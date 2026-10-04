// fix #5033 (v0.315) / §14.1 / §14.2 / §2.8.3 rule 4 / §12
//
// REGRESSION COVERAGE for issue #5033: the #5009 fail-closed gate was too
// broad — it rejected legal cross-module virtual generic method calls.
//
// THE BUG. `importedGenericLacksConstraintBody5009` in LlvmBackend_Calls2.cpp
// rejected every imported generic virtual method with a `new()` constraint,
// regardless of whether the generic parameter appeared in the signature. The
// predicate was: if constraint == new() -> reject.
//
// THE COST. For shapes where T does NOT appear in a parameter type (the VOnly
// and VPlain variants), the body computes a constant at any instantiation
// (new T() at concrete type is emitted by the caller's monomorphization), so
// the abstract template body is never invoked. The placeholder template
// produces the same result as the concrete one — no width mismatch, no
// silent wrong code. The gate was a false positive.
//
// THE FIX. Narrow the `new()` arm to only reject when the generic parameter
// appears in a parameter type. When T is absent from the signature, the
// placeholder is sound.
//
// SHAPE TABLE (from spec analysis):
//   VOnly    : T only in return type (not in params), new() only in body → FIXED
//   VPlain   : T in body only (not in params), new() only in body → FIXED
//   VBDirect : T in parameter, type constraint → still rejected (correct)
//   VBAppInh : T in parameter (base type), type constraint → still rejected (correct)
//   VBAppSub : T in parameter (substituted type), type constraint → still rejected (correct)
//   VPBase   : non-generic base, generic subclass override → still accepted (correct)
//   VPSubDirect: non-generic base, generic override with T not in params → still accepted (correct)
//
// FAIL-WITHOUT: revert the fix to `importedGenericLacksConstraintBody5009`, VOnly
// and VPlain cross-module calls are rejected with a "constraint requires body
// monomorphization" diagnostic.
//
// PASS-WITH: VOnly returns 96, VPlain returns 101.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

using bezel::test::runAotChecked;

// --- Cross-module harness --------------------------------------------------
//
// Uses Driver::compileObject + Driver::compileFile with a Dependency — the
// same shape #3082 / #3228 / #2844 / #2845 use for their import-side oracle.

const std::filesystem::path& wtDir5033() {
    return wtScratchDir("wt-5033-5033");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

int runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& name,
                   const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    h.src = std::make_shared<TempFile>((dir / (name + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (name + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& name,
                   const std::filesystem::path& dir,
                   const std::string& libObjPath,
                   const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    r.src = std::make_shared<TempFile>((dir / (name + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (name + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath   = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// ===========================================================================
// T1 — VOnly (virtual generic, T in return type, new() constraint, T NOT in
// params). Before fix: REJECTED. After fix: ACCEPTED, returns 96.
//
// Shape:
//   Dep: public virtual method VOnly<T>(): int where T: new() { var t := new T(); return 96; }
//   App: var g := new G(); return g.VOnly<R>();
// ===========================================================================
BEZEL_TEST(virtual_generic_new_constraint_cross_module_5033,
           vonly_cross_module_T_not_in_params_spec_14_1) {
    const auto& dir = wtDir5033();
    const std::string libSrc =
        "module VOnlyDep; "
        "public class G "
        "  public constructor() end constructor; "
        "  public virtual method VOnly<T>(): int where T: new() "
        "    var t := new T(); "
        "    return 96; "
        "  end VOnly; "
        "end G; ";
    const std::string appSrc =
        "module VOnlyApp;\n"
        "import VOnlyDep;\n"
        "public class R\n"
        "  public value: int;\n"
        "  public constructor() end constructor;\n"
        "end R;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var g := new G();\n"
        "    return g.VOnly<R>();\n"
        "  end Main;\n"
        "end C; ";

    LibHandle libH = buildLib("VOnly", dir, libSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp("VOnly", dir, libH.objPath, appSrc);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    int code = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(code, 96);
}

// ===========================================================================
// T2 — VPlain (virtual generic, T in body only, new() constraint, T NOT in
// params). Before fix: REJECTED. After fix: ACCEPTED, returns 101.
//
// Shape:
//   Dep: public virtual method VPlain<T>(x: int): int where T: new() { return 96 + x; }
//   App: var g := new GPlain(); return g.VPlain<R>(5);
// ===========================================================================
BEZEL_TEST(virtual_generic_new_constraint_cross_module_5033,
           vplain_cross_module_T_not_in_params_spec_14_1) {
    const auto& dir = wtDir5033();
    const std::string libSrc =
        "module VPlainDep; "
        "public class GPlain "
        "  public constructor() end constructor; "
        "  public virtual method VPlain<T>(x: int): int where T: new() "
        "    return 96 + x; "
        "  end VPlain; "
        "end GPlain; ";
    const std::string appSrc =
        "module VPlainApp;\n"
        "import VPlainDep;\n"
        "public class R\n"
        "  public value: int;\n"
        "  public constructor() end constructor;\n"
        "end R;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var g := new GPlain();\n"
        "    return g.VPlain<R>(5);\n"
        "  end Main;\n"
        "end C; ";

    LibHandle libH = buildLib("VPlain", dir, libSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp("VPlain", dir, libH.objPath, appSrc);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    int code = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(code, 101);  // 96 + 5
}

// ===========================================================================
// T3 — VBDirect CONTROL (virtual generic, T in parameter, type constraint).
// Still correctly REJECTED after fix — T appears in parameter type, width
// mismatch possible.
//
// Shape:
//   Dep: public virtual method VBDirect<T>(v: T): int where T: IShapeVB { return 96; }
//   App: var sq := new Square(); var g := new GVB(); return g.VBDirect<Square>(sq);
// ===========================================================================
BEZEL_TEST(virtual_generic_new_constraint_cross_module_5033,
           vbdirect_cross_module_T_in_param_still_rejected_spec_14_1) {
    const auto& dir = wtDir5033();
    const std::string libSrc =
        "module VBDirectDep; "
        "public interface IShapeVB "
        "  method Area(): int; "
        "end IShapeVB; "
        "public class GVB "
        "  public constructor() end constructor; "
        "  public virtual method VBDirect<T>(v: T): int where T: IShapeVB "
        "    return 96; "
        "  end VBDirect; "
        "end GVB; ";
    const std::string appSrc =
        "module VBDirectApp;\n"
        "import VBDirectDep;\n"
        "public class Square\n"
        "  public side: int;\n"
        "  public constructor(s: int) side := s; end constructor;\n"
        "  public method Area(): int return this.side * this.side; end Area;\n"
        "end Square;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var sq := new Square(5);\n"
        "    var g := new GVB();\n"
        "    return g.VBDirect<Square>(sq);\n"
        "  end Main;\n"
        "end C; ";

    LibHandle libH = buildLib("VBDirect", dir, libSrc);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp("VBDirect", dir, libH.objPath, appSrc);
    // Must be rejected — T appears in parameter type, type constraint requires
    // body monomorphization, and the body is not shipped for imported methods.
    BEZEL_EXPECT_FALSE(appH.ok);
}

// ===========================================================================
// T4 — CONTROL: single-module VOnly still works (same-module always worked).
// This is the baseline that both pre-fix and post-fix pass — it proves the
// shape itself is legal Bezel code.
// ===========================================================================
BEZEL_TEST(virtual_generic_new_constraint_cross_module_5033,
           vonly_single_module_control_spec_14_1) {
    const std::string src =
        "module H; "
        "public class G "
        "  public constructor() end constructor; "
        "  public virtual method VOnly<T>(): int where T: new() "
        "    var t := new T(); "
        "    return 96; "
        "  end VOnly; "
        "end G; "
        "public class R "
        "  public value: int; "
        "  public constructor() end constructor; "
        "end R; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new G(); "
        "    return g.VOnly<R>(); "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 96);
}

}  // namespace
}  // namespace bezel::codegen
