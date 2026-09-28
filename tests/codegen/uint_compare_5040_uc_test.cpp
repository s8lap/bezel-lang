// Codegen — fix #5040: cross-module: a generic class method whose body
// invokes a STDLIB constraint interface method (`where T: IComparable<T>`
// and the body calls `a.CompareTo(b)`) must produce a T→T correct compare
// when T is an UNSIGNED primitive (`uint`/`ulong`). The cross-module
// specialization gate at LlvmBackend_Calls2.cpp:15065 fires for an imported
// method call into a generic parent, but it can only synthesize the
// per-instantiation body if the importer successfully re-attaches the
// LIB's `MethodDecl` — which §14.1 / #783 requires the synthetic source
// to express the class's WHERE-clause constraints. Without that, the body
// typecheck fails on `T.CompareTo(b)` (T is unconstrained in the fresh
// context), the loader declines to attach the decl, the gate does not
// fire, and the importer-side call falls through to the LIB's i32
// placeholder body — which is a SIGNED `icmp sgt`, so for `T=uint` and
// (a,b)=(4000000000u, 1u) the compare reports a < b (1u is the "max")
// and the method returns `b` rather than `a`.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// Pre-fix, the imported `MaxC<uint>::Compare` (T=uint, the most damaging
// combination because uint's high bit is 1 for any value ≥ 2^31 — i.e.
// `icmp sgt` flips the result) returned `b` for (4000000000u, 1u),
// producing exit code 200 from the app's "if max == 4000000000u then 100
// else 200". The single-module twin of the SAME `class MaxC<T>` correctly
// returned 100 — both because the import path is not involved AND because
// the local emit-side substitution carries uint through correctly. The
// defect is specific to the cross-module path that the gate was
// designed to fix.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// ModuleLoader.cpp's `collectRenderableConstraintTypes` previously declined
// (returned false) for two patterns: (1) non-sibling constraint types
// (e.g. a STDLIB/BUILTIN interface like `IComparable<T>`), and (2)
// constraint types with non-empty `genericArgs` (e.g. `IComparable<T>`)
// — both because the synthetic source has no natural way to *name* the
// type and no rendering rule for the arg list. The fix removes both
// declines: the fresh context's prelude-and-class-header together can
// resolve `IComparable<T>` (the interface is builtin, and T is the class's
// own param so the synthetic source's header provides it). The subsequent
// typecheck still gates the body — a body that names a non-renderable
// generic arg (e.g. an unrendered sibling's own param) fails to resolve
// and the §14.1/#783 soundness gate declines as before.
//
// ── SPEC CITATION ──────────────────────────────────────────────────────────
//
// §14.1 "Generic declarations" / §14.2 "Where-clause constraints" / §2.5
// "Dependency resolution" / §2.8.2 "The artifact interface is a semantic
// requirement" / §2.8.3 rule 4 (per-instance monomorphization); stdlib
// §2.8 `IComparable<T>`, v0.307. The §14.1 soundness rule is the one that
// gates `parseAndAttachMethodBody` — it is the contract the loader must
// satisfy for the cross-module method body to be re-attachable.
//
// ── FAIL-WITHOUT / PASS-WITH (verified mechanically on a23-5040-uc) ───────
//
//  T1 imported MaxC<uint> Compare of (4000000000u, 1u):  200 pre-fix, 100 post-fix.
//  T2 same-module twin (control):                       100 pre-fix, 100 post-fix.
//
// The T2 twin locks in the local path: the local emit-side substitution
// is correct both before and after the fix, so the post-fix run cannot
// have widened a wider diagnostic gate to mask the fix.
//
// ── SCOPE / NOT-COVERED ────────────────────────────────────────────────────
//
// This file exercises the cross-module `where T: IComparable<T>` +
// `a.CompareTo(b)` + unsigned-T shape. The same defect class includes
// `IEquatable<T>`/`.Equals(...)` (uint's Equals was never broken — its
// body is sign-agnostic — but a generic class body that names it via the
// same import path was incorrectly REJECTED by the §14.1 soundness gate
// for the same reason), `where T: IComparable<T>` with class-arg T (Cell,
// Ptr: an interface T works in single-module but was rejected cross-
// module pre-fix; not exercised here), and the T→T → T → T case (chained
// constraints). All three are fixed by the same `collectRenderableConstraintTypes`
// widening but each merits its own dedicated test row when pursued.

#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"

namespace fs = std::filesystem;

namespace {

struct TempFile5040 {
    std::string path;
    explicit TempFile5040(std::string p) : path(std::move(p)) {}
    ~TempFile5040() { std::remove(path.c_str()); }
};

void writeSource5040(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// A generic class with an `IComparable<T>`-constrained `Compare` method
// that picks the larger of two T values. Pre-fix, an IMPORTED instantiation
// at T=uint routes through the LIB's i32 placeholder body (a signed
// `icmp sgt`), which is wrong for any uint value ≥ 2^31.
struct LibHandle5040 {
    std::shared_ptr<TempFile5040> src;
    std::shared_ptr<TempFile5040> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle5040 buildLib5040(const fs::path& dir, const std::string& libSrc) {
    LibHandle5040 h;
    std::string stem = unique_stem("bz5040lib_");
    h.src = std::make_shared<TempFile5040>(
        (dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile5040>(
        (dir / (stem + "_lib.o")).string());
    writeSource5040(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r =
        driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle5040 {
    std::shared_ptr<TempFile5040> src;
    std::shared_ptr<TempFile5040> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle5040 buildApp5040(const fs::path& dir,
                           const std::string& libObjPath,
                           const std::string& appSrc) {
    AppHandle5040 r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bz5040app_");
    r.src = std::make_shared<TempFile5040>(
        (dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile5040>(
        (dir / (stem + "_app")).string());
    writeSource5040(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

struct RunResult5040 { int code; std::string out; };

RunResult5040 runCapture5040(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Per-test workdir under the shared scratch base. §2.4/§2.8 dependency
// resolution requires per-call scratch isolation; the shared /tmp tmpfs
// races across shards.
static const fs::path& wtDir5040() {
    return wtScratchDir("wt-5040-uc");
}

}  // namespace

// ===========================================================================
// T1 — issue's HEADLINE repro, cross-module. `class MaxC<T> where T:
// IComparable<T>` declares a `Compare(): T` method whose body calls
// `a.CompareTo(b)`. The LIB exports the method against an i32 placeholder
// (the erasure in the artifact interface, §2.8.2); the IMPORTER must
// re-specialize the body per-T via the cross-module gate at
// LlvmBackend_Calls2.cpp:15065. For T=uint with (a,b)=(4000000000u, 1u)
// the correct result is `a` (4000000000u is the larger of the two
// values). Pre-fix the importer's gate did not fire (because the loader's
// `parseAndAttachMethodBody` declined to re-attach the MethodDecl — its
// §14.1 soundness gate rejected the body as typechecking T.CompareTo(b)
// against an unconstrained T in the fresh context, since the synthetic
// source lacked the `where T: IComparable<T>` clause). The call fell
// through to the i32 placeholder body, a SIGNED compare (`icmp sgt`),
// which returned 1 for the comparison (a < b in signed) so the method
// chose `b`. Exit code 200.
// ===========================================================================
BEZEL_TEST(uint_compare_5040_uc,
           t1_xmod_generic_compare_unsigned_returns_max_14_1) {
    auto dir = wtDir5040();
    const std::string lib =
        "module Lib5040t1; "
        "public class MaxC<T> where T: IComparable<T> "
        "  public a: T; "
        "  public b: T; "
        "  public constructor(x: T, y: T) "
        "    a := x; b := y; "
        "  end constructor; "
        "  public method Compare(): T "
        "    if a.CompareTo(b) > 0 then return a; else return b; end; "
        "  end Compare; "
        "end MaxC; ";
    const std::string app =
        "module App5040t1; "
        "import Lib5040t1; "
        "public class C "
        "  public static method Main(): int "
        "    var m := new MaxC<uint>(4000000000u, 1u); "
        "    var r: uint := m.Compare(); "
        "    if r = 4000000000u then return 100; end; "
        "    return 200; "
        "  end Main; "
        "end C; ";
    LibHandle5040 libH = buildLib5040(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle5040 appH = buildApp5040(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    auto [code, out] = runCapture5040(appH.exePath);
    BEZEL_EXPECT_EQ(code, 100);
}

// ===========================================================================
// T2 — same-module control. The SAME `class MaxC<T>` declared in the same
// module as the Main caller must continue to return 100 across the fix.
// If this regressed, the fix would have widened the local path's
// semantics (the dangerous direction); locking the twin in at the post-
// fix value pins the differential control.
// ===========================================================================
BEZEL_TEST(uint_compare_5040_uc,
           t2_same_module_compare_unsigned_returns_max_14_1) {
    auto dir = wtDir5040();
    const std::string same =
        "module Same5040t2; "
        "public class MaxC<T> where T: IComparable<T> "
        "  public a: T; "
        "  public b: T; "
        "  public constructor(x: T, y: T) "
        "    a := x; b := y; "
        "  end constructor; "
        "  public method Compare(): T "
        "    if a.CompareTo(b) > 0 then return a; else return b; end; "
        "  end Compare; "
        "end MaxC; "
        "public class C "
        "  public static method Main(): int "
        "    var m := new MaxC<uint>(4000000000u, 1u); "
        "    var r: uint := m.Compare(); "
        "    if r = 4000000000u then return 100; end; "
        "    return 200; "
        "  end Main; "
        "end C; ";
    std::string stem = unique_stem("bz5040ctl_");
    auto src = std::make_shared<TempFile5040>(
        (dir / (stem + ".bzl")).string());
    auto exe = std::make_shared<TempFile5040>(
        (dir / stem).string());
    writeSource5040(src->path, same);
    bezel::Driver driver;
    bezel::CompileResult r =
        driver.compileFile(src->path, exe->path, {});
    BEZEL_EXPECT_TRUE(r.success);
    if (!r.success) return;
    auto [code, out] = runCapture5040(exe->path);
    BEZEL_EXPECT_EQ(code, 100);
}
