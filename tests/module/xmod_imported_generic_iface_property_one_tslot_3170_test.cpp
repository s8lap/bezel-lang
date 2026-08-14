// fix #3170 (§2.8.2 clause 2 / §14.1 / §2.8.3 rule 4 / §13 / §23 / §12.1,
// v0.254). The one-`T`-slot case #2943's gate deliberately does not cover.
//
// THE DEFECT. A property read on an IMPORTED GENERIC class reached through
// an INTERFACE-typed reference SIGSEGVs when the instantiation's storage
// layout moves relative to the erased `T -> i32` template the exporter
// compiled against. The codegen-side partial fix in #2943 / #2894 keeps
// the itable property-accessor slot NULL on a layout-moving instantiation
// — the only alternative (installing the template accessor) would silently
// produce wrong values — so the dispatch through the itable reads address
// 0x0 and crashes.
//
// THE ONE-`T`-SLOT CASE. #2943's sema gate fired only on classes with >=2
// T-typed fields; it reasoned that a single T field's i32 placeholder and
// the substituted ptr "share one 8-byte-aligned slot, so `_w` lands at the
// same offset either way". Measured on #3170's parent:
//
//     `class G<T> { _t: T; _w: int; }` instantiated at `T = string`
//     reached through `var r: IHas := g; r.W`  →  exit 139 (SIGSEGV)
//
// `_w` is at offset 8 in the erased template (`{i32 _t, i32 _w}`) and at
// offset 16 in the spec layout (`{ptr _t, i32 _w}`); the comment's "same
// offset either way" claim is wrong. #2943's predicate is therefore too
// narrow: it under-approximates the layout-move condition, and the
// one-`T`-slot case silently SIGSEGVs.
//
// THE FIX widens #2943's predicate (d) from ">=2 T-typed fields" to
// "some T-typed field followed in declaration order by a non-T-typed
// field". That is the exact layout-move discriminator #3170 names: a T
// field followed by a concrete field moves the concrete field's offset
// the moment T widens past i32, regardless of how many other T fields
// there are.
//
// OVER-REJECTION COST. The sema gate has no view of the concrete type
// argument at the access site (the static type is the interface), so
// the predicate is a whole-program over-approximation: it fires for
// every shape where SOME instantiation moves the layout. The 2894 T1
// control case (`GIface2894T1<int>` with `_t: T; _w: int` instantiated
// at `T = int`) used to be accepted because the substituted layout
// matches the erased template at `int`; it is now declined here,
// mirroring the documented "value vs crash" trade #2884 made for the
// direct-class path. The over-rejection is strictly better than the
// silent crash it replaces; the residual is the #2893 / #428 importer-
// side accessor-body monomorphization work that #2884 and #2943
// deferred.
//
// AXIS GRID:
//   * T-field count:    1 (the issue's own shape), 2 (the #2943 control
//                       case, pinned here so the widening doesn't drop
//                       it), 0 (the layout-identical control, pinned
//                       here so the widening doesn't over-reject)
//   * Instantiation:    T = string (layout-moving, the issue's repro)
//                       and T = int (layout-identical, the control that
//                       the over-rejection costs)
//   * Module shape:     two-module (imported, in gate) and
//                       single-module twin (control, NOT in gate)
//   * Receiver class:   implementing class shape (cross-module, in gate)
//                       and interface-typed reference (in gate)
//
// Re-pro test cases. The interface is non-generic; the implementing class
// is imported generic; the access is via interface reference (itable-
// mediated). Pre-fix the program compiles AND links clean AND SEGVs at
// runtime (exit 139); post-fix the program declines at the access site
// with a §-cited diagnostic that names #3170.
//
// Adjacent suites green: xmod_imported_generic_iface_property_layout_movable_2943
// (the two-T-field case is still declined; T3/T4/T5 controls unchanged)
// and the imported-generic iface-property test
// xmod_imported_generic_iface_accessor_2894 T2 (no-T-field layout-
// identical control still passes).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile3170 {
    std::string path;
    explicit TempFile3170(std::string p) : path(std::move(p)) {}
    ~TempFile3170() { std::remove(path.c_str()); }
};

void writeSource3170(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3170(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Same shape as the #2943 helper: compile lib + app against the lib,
// capturing stderr so the diagnostic text can be asserted (pre-fix these
// programs COMPILE CLEAN, so "compile failed" is not evidence of
// anything — the bug was a runtime SEGV, which the codegen-side partial
// fix made into the deliberate null slot).
struct BuildResult3170 {
    bool depOk;     // false => toolchain unavailable, SKIP
    bool success;
    std::string err;
};

BuildResult3170 compileImportCaptureErr3170(
    const std::string& libSrc,
    const std::string& appSrc,
    const std::string& tag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3170_" + tag + "_" + std::to_string(std::rand());
    TempFile3170 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3170 aObj((dir / (stem + "_a.o")).string());
    writeSource3170(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile3170 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3170 bExe((dir / stem).string());
    writeSource3170(bSrc.path, appSrc);

    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    std::remove(bExe.path.c_str());
    return {true, success, err};
}

bool hasFragment3170(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Diagnostic fragments asserted (rule identifier + key terms — diagnostic
// rewording that doesn't drop any of these preserves the pin). Cites
// #3170 explicitly — the new identifier the #3170 widening adds to the
// diagnostic text.
void expectDecline3170(const std::string& err, const std::string& prop,
                       const std::string& iface) {
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "property `" + prop + "`"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "imported interface `" + iface + "`"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "not yet monomorphized"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "#2943"));
    // The new identifier the widening adds — the issue this lane closes.
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "#3170"));
    // §-citations from the issue (spec §2.8.2 clause 2 is the authority
    // the implementation contradicts; §14.1 / §2.8.3 rule 4 / §13 /
    // §23 are the same citations #2943 already names).
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "\xC2\xA7""14.1"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "\xC2\xA7""2.8.3 rule 4"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "\xC2\xA7""13"));
    BEZEL_EXPECT_TRUE(hasFragment3170(err, "\xC2\xA7""23"));
}

// Build + run a two-module program. Returns the program's exit code, or
// -1 if the build fails (which post-fix is the EXPECTED outcome for the
// one-`T`-slot layout-moving case — the sema gate now refuses it).
int compileImportAndRun3170(
    const std::string& libSrc,
    const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3170run_" + std::to_string(std::rand());
    TempFile3170 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3170 aObj((dir / (stem + "_a.o")).string());
    writeSource3170(aSrc.path, libSrc);
    if (!compileToObj3170(aSrc.path, aObj.path)) return -1;
    if (!std::ifstream(aObj.path).good()) return -1;

    TempFile3170 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3170 bExe((dir / stem).string());
    writeSource3170(bSrc.path, appSrc);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success)
            return -2;
    }
    int status = std::system(bExe.path.c_str());
    std::remove(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO, property GETTER on an IMPORTED generic
// class with ONE `T`-typed storage slot, reached through an
// interface-typed reference. Pre-fix: exit 139 (SIGSEGV through the
// null itable slot). Post-fix: compile-time decline with the
// §-cited diagnostic.
//
// ONE T-typed field (`_t: T;`) before `_w: int`, instantiated at
// `T = string`. The codegen-side
// `instanceLayoutDiffersFromErasedTemplate` returns true at this
// instantiation: `{i32 _t, i32 _w}` (erased) vs `{ptr _t, i32 _w}`
// (spec) — `_w` moves from offset 8 to offset 16. The itable slot
// stays null, so the dispatch faults with a SIGSEGV. The sema-side
// decline converts that crash into a sourced diagnostic at the
// access site.
//
// §2.8.2 clause 2 (an external instantiation behaves identically to
// a same-module instantiation of the same generic at the same
// arguments) is the authority the implementation contradicts: the
// cross-module dispatch SIGSEGVs while the same-module twin prints
// the correct value. §14.1 / §2.8.3 rule 4 / §13 / §23 / §12.1
// are the narrower citations #2943 already names. v0.254.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_one_tslot_3170,
           issue_repro_one_tslot_declined_spec_2_8_2_clause_2) {
    auto r = compileImportCaptureErr3170(
        "module IReproLib3170T1; "
        "public interface IHas3170T1 "
        "  property W: int get; end; "
        "end IHas3170T1; "
        "public class GOne3170T1<T>: IHas3170T1 "
        "  public _t: T; "
        "  public _w: int := 0; "
        "  public property W: int get => _w + 100; end; "
        "end GOne3170T1;",
        "module IReproApp3170T1; import IReproLib3170T1; "
        "public class M3170T1 "
        "  public static method Main(): int "
        "    using var g := new GOne3170T1<string>(); "
        "    g._w := 7; "
        "    var r: IHas3170T1 := g; "
        "    var x: int := r.W; "
        "    return 0; "
        "  end Main; "
        "end M3170T1;",
        "t1");
    if (!r.depOk) return;  // toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline3170(r.err, "W", "IHas3170T1");
}

// ===========================================================================
// T2 — LAYOUT-IDENTICAL CONTROL. The implementing class has NO
// T-typed storage at all, so the codegen-side
// `instanceLayoutDiffersFromErasedTemplate` is false at every
// instantiation, and the itable slot IS filled. The property access
// returns the correct value (107 = 7 + 100). The widening must NOT
// decline this case — the predicate fires only on "T field followed
// by a non-T field", and a class with zero T fields has no T field
// to begin with. Pinned by running the program end-to-end and
// asserting the exit code equals 107.
//
// This is the boundary case for (d): a class with 0 T-typed fields
// is layout-IDENTICAL even at `T = string`, and the dispatch path
// returns the correct value. §13 / §23 / §2.5, v0.254.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_one_tslot_3170,
           layout_identical_no_t_fields_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3170ct2_" + std::to_string(std::rand());
    TempFile3170 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3170 aObj((dir / (stem + "_a.o")).string());
    writeSource3170(aSrc.path,
        "module CtlLib3170T2; "
        "public interface ICtl3170T2 "
        "  property W: int get; end; "
        "end ICtl3170T2; "
        "public class GCtl3170T2<T>: ICtl3170T2 "
        "  public _w: int := 7; "
        "  public property W: int get => _w + 100; end; "
        "end GCtl3170T2;");
    if (!compileToObj3170(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile3170 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3170 bExe((dir / stem).string());
    writeSource3170(bSrc.path,
        "module CtlApp3170T2; import CtlLib3170T2; "
        "public class MCtl3170T2 "
        "  public static method Main(): int "
        "    using var g := new GCtl3170T2<string>(); "
        "    var r: ICtl3170T2 := g; "
        "    return r.W; "
        "  end Main; "
        "end MCtl3170T2;");
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success) {
            BEZEL_EXPECT_TRUE(false);  // layout-identical must build clean
            return;
        }
    }
    int status = std::system(bExe.path.c_str());
    std::remove(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 107);  // 7 + 100
}

// ===========================================================================
// T3 — TWO-T-FIELD CONTROL (#2943's original case). The implementing
// class has TWO T-typed fields (`_t: T; _t2: T;`) followed by
// `_w: int`, instantiated at `T = string`. This was already declined
// by #2943; the widening must keep it declined (it satisfies the new
// predicate too, because the LAST T field `_t2` is followed by `_w`,
// a non-T field). The diagnostic cites BOTH #2943 AND #3170 — the
// widening's identifier is added, the old one is preserved.
//
// Pinned by the SAME assertion T1 uses (build fails with the
// §-cited diagnostic). §14.1 / §2.8.3 rule 4 / §13 / §23,
// v0.254.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_one_tslot_3170,
           two_t_fields_still_declined_spec_14_1) {
    auto r = compileImportCaptureErr3170(
        "module CtlLib3170T3; "
        "public interface IHas3170T3 "
        "  property W: int get; set; end; "
        "end IHas3170T3; "
        "public class GTwo3170T3<T>: IHas3170T3 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GTwo3170T3;",
        "module CtlApp3170T3; import CtlLib3170T3; "
        "public class M3170T3 "
        "  public static method Main(): int "
        "    using var g := new GTwo3170T3<string>(); "
        "    var r: IHas3170T3 := g; "
        "    var x: int := r.W; "
        "    return 0; "
        "  end Main; "
        "end M3170T3;",
        "t3");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline3170(r.err, "W", "IHas3170T3");
}

// ===========================================================================
// T4 — DIFFERENT MEMBER KIND: an interface METHOD call on the SAME
// one-`T`-slot layout-moving class as T1. The method slot is filled
// by the codegen-side #2943 partial fix (a re-lowered specialization
// under the active substitution), so the dispatch returns the
// correct value 42. The sema-side predicate (d) finds the same
// T-field-followed-by-concrete-field class — but the decline is
// keyed at property access (`!ips->isIndexer`), so a method call on
// the same receiver MUST NOT be declined. This is the cross-surface
// coverage the lane brief asks for: the decline fires on properties
// and not on methods, even on the shape the widening catches.
//
// Pinned by running the program end-to-end and asserting rc=42.
// §13 / §2.8.3 rule 4, v0.254.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_one_tslot_3170,
           method_call_on_same_one_tslot_layout_moving_class_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3170ct4_" + std::to_string(std::rand());
    TempFile3170 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3170 aObj((dir / (stem + "_a.o")).string());
    writeSource3170(aSrc.path,
        "module MthLib3170T4; "
        "public interface IDo3170T4 "
        "  method Twice(): int; "
        "end IDo3170T4; "
        "public class GDo3170T4<T>: IDo3170T4 "
        "  public _t: T; "         // ONE T-typed field (the widening's shape)
        "  public _w: int := 21; " // non-T field after — moves the layout
        "  public method Twice(): int return _w * 2; end Twice; "
        "end GDo3170T4;");
    if (!compileToObj3170(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile3170 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3170 bExe((dir / stem).string());
    writeSource3170(bSrc.path,
        "module MthApp3170T4; import MthLib3170T4; "
        "public class MMth3170T4 "
        "  public static method Main(): int "
        "    using var g := new GDo3170T4<string>(); "
        "    var d: IDo3170T4 := g; "
        "    return d.Twice(); "
        "  end Main; "
        "end MMth3170T4;");
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success) {
            BEZEL_EXPECT_TRUE(false);  // method call over-rejected
            return;
        }
    }
    int status = std::system(bExe.path.c_str());
    std::remove(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// ===========================================================================
// T5 — SINGLE-MODULE TWIN CONTROL. The byte-identical program compiled
// in a single module (no `import`) dispatches the same interface
// property through the same code path and returns the correct value
// 107 (7 + 100). This pins that the issue's defect is a CROSS-MODULE
// artifact — the same-source single-module build was always correct —
// and the sema gate's predicate deliberately excludes the local
// case (it fires only when the interface is IMPORTED, clause (b)).
//
// Pinned by running the program end-to-end and asserting rc=107.
// §2.8.2 clause 2, v0.254.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_one_tslot_3170,
           single_module_twin_returns_107_spec_2_8_2_clause_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3170ct5_" + std::to_string(std::rand());
    TempFile3170 src((dir / (stem + ".bzl")).string());
    TempFile3170 exe((dir / stem).string());
    writeSource3170(src.path,
        "module Twin3170T5; "
        "public interface IHas3170T5 "
        "  property W: int get; end; "
        "end IHas3170T5; "
        "public class G3170T5<T>: IHas3170T5 "
        "  public _t: T; "
        "  public _w: int := 0; "
        "  public property W: int get => _w + 100; end; "
        "end G3170T5; "
        "public class M3170T5 "
        "  public static method Main(): int "
        "    using var g := new G3170T5<string>(); "
        "    g._w := 7; "
        "    var r: IHas3170T5 := g; "
        "    return r.W; "
        "  end Main; "
        "end M3170T5;");
    {
        bezel::Driver driver;
        if (!driver.compileFile(src.path, exe.path, {}).success) {
            BEZEL_EXPECT_TRUE(false);  // single-module must build clean
            return;
        }
    }
    int status = std::system(exe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 107);  // 7 + 100
}

}  // namespace bezel::module
