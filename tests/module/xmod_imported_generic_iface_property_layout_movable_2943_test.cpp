// fix #2943 (§14.1 / §2.8.3 rule 4 / §13 / §23 / §12.1, v0.240,
// decline-with-reasons, sema-side arm).
//
// THE DEFECT. #2894 populated the per-instantiation itmap of an IMPORTED
// GENERIC class but gated the whole table on the instantiation's storage
// layout being identical to the erased `T -> i32` template the exporter
// compiled against — a layout-moving instantiation kept the pre-#2894
// behaviour, an empty itmap, so an interface dispatch on it faulted. #2943's
// codegen-side partial fix (in main at 09235fe) narrowed the gate from
// per-TABLE to per-SLOT-KIND — METHOD slots are re-lowered under THIS
// substitution via `buildSpecializedMethodFn`, but ACCESSOR slots remain
// deliberately NULL on a layout-moving instantiation, so `r.W` on
// `var r: IHas := new GIface<string>()` faults at the first dispatch
// through the itable. Measured on this fix's parent (real two-module build,
// exit 139 = SIGSEGV; the codegen-side partial fix is in main and the
// method arm now returns its correct value — the property arm is the
// remaining gap).
//
// THE FIX is a SEMa-side decline, converting the SEGV into a sourced
// diagnostic at the access site. The full capability — importer-side
// accessor-body monomorphization per instantiation, the way
// `methodBodyText` / `ctorBodyText` already carry method and constructor
// bodies across the artifact boundary — is the #428 core, deferred. The
// decline is recorded as one (CLAUDE.md rule 4) and strictly better than
// the silent SEGV.
//
// NARROW PREDICATE (§14.1 / §23 — "predicates are precise, not broad"),
// mirroring the #2884 / #2955 / #2928 family. The decline fires only when
// every one of these holds:
//   (a) the accessed property is declared on an interface, not inherited
//       from a class or record;
//   (b) the interface is imported (cross-module codegen path, not local
//       inlining which is OUT OF GATE);
//   (c) the interface is NOT generic — a generic interface with a T-typed
//       property return is the #2944 family (Lane 1's narrower predicate),
//       and a generic interface's indexer is #2955's separate decline;
//   (d) at least one imported GENERIC CLASS implementing this interface
//       declares a field whose resolved TypeRef is a bare name matching
//       one of the class's own generic parameters — i.e., has the
//       layout-moving potential the codegen-side decline (`importedLayout
//       Moves`) already keys on. Without (d) a layout-identical
//       instantiation like `class G<T>: IFoo { _w: int := 21; property P:
//       int get => _w + 100; end; }` (the T6 control shape) would be
//       over-rejected.
// Codegen's `importedLayoutMoves` is an exact layout check
// (instanceLayoutDiffersFromErasedTemplate); (d) is its narrowest
// sema-time approximation. The over-rejection cost is a diagnostic at
// the access site instead of a value, which is the documented "value vs
// crash" trade #2884 made.
//
// Re-pro test cases. The interface is non-generic, the implementing class
// is imported generic with T-typed storage, the access is via interface
// reference (so dispatch is itable-mediated), and the property's body
// reads fields whose offset moves under the substitution. Pre-fix the
// program compiles AND links clean, AND SEGVs at runtime; post-fix the
// program declines at the access site with a §-cited diagnostic.
//
// Adjacent suites green: xmod (the existing #2943 codegen-side test
// suite still passes — its T3 was deliberately pinned as a RANGE
// accepting EITHER -1 (today's SEGV) OR 108 (correct value), and the
// sema decline makes -1 the only observed outcome), generic, import,
// module, iface.

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

struct TempFile2943Decline {
    std::string path;
    explicit TempFile2943Decline(std::string p) : path(std::move(p)) {}
    ~TempFile2943Decline() { std::remove(path.c_str()); }
};

void writeSource2943Decline(const std::string& path,
                            const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2943Decline(const std::string& src,
                             const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib + app against the lib, capturing stderr so the diagnostic
// text can be asserted (pre-fix these programs COMPILE CLEAN, so "compile
// failed" is not evidence of anything — the bug was a runtime SEGV, which
// the codegen-side partial fix made into the deliberate null slot).
struct BuildResult2943 {
    bool depOk;     // false => toolchain unavailable, SKIP
    bool success;
    std::string err;
};

BuildResult2943 compileImportCaptureErr2943(
    const std::string& libSrc,
    const std::string& appSrc,
    const std::string& tag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2943d_" + tag + "_" + std::to_string(std::rand());
    TempFile2943Decline aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943Decline aObj((dir / (stem + "_a.o")).string());
    writeSource2943Decline(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile2943Decline bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943Decline bExe((dir / stem).string());
    writeSource2943Decline(bSrc.path, appSrc);

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

bool hasFragment2943(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Diagnostic fragments asserted (rule identifier + key terms — diagnostic
// rewording that doesn't drop any of these preserves the pin).
void expectDecline2943(const std::string& err, const std::string& prop,
                       const std::string& iface) {
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "property `" + prop + "`"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "imported interface `" + iface + "`"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "not yet monomorphized"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "#2943"));
    // §-citations from the issue.
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "\xC2\xA7""14.1"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "\xC2\xA7""2.8.3 rule 4"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "\xC2\xA7""13"));
    BEZEL_EXPECT_TRUE(hasFragment2943(err, "\xC2\xA7""23"));
}

}  // namespace

// ===========================================================================
// T1 — issue's own repro, accessor getter on a layout-moving imported
// generic. Pre-fix: exit 139 (SIGSEGV through the null itable slot).
// Post-fix: compile-time decline with the §-cited diagnostic.
// §14.1 / §2.8.3 rule 4 / §13 / §23 / §12.1, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           issue_repro_getter_declined_spec_14_1) {
    auto r = compileImportCaptureErr2943(
        "module IReproLib2943T1; "
        "public interface IHas2943T1 "
        "  property W: int get; set; end; "
        "end IHas2943T1; "
        "public class GIface2943T1<T>: IHas2943T1 "
        "  public _t: T; "
        "public _t2: T; "
        "public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GIface2943T1;",
        "module IReproApp2943T1; import IReproLib2943T1; "
        "public class M2943T1 "
        "  public static method Main(): int "
        "    using var g := new GIface2943T1<string>(); "
        "    var r: IHas2943T1 := g; "
        "    var x: int := r.W; "
        "    return 0; "
        "  end Main; "
        "end M2943T1;",
        "t1");
    if (!r.depOk) return;  // toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2943(r.err, "W", "IHas2943T1");
}

// ===========================================================================
// T2 — setter on the same shape; the issue reported the getter SEGV, but
// the setter twin SEGVs the same way (a separate itable slot, the same
// layout-move predicate, the same deliberate null). Both must decline.
// §23.3 / §14.1 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           issue_repro_setter_declined_spec_23_3) {
    auto r = compileImportCaptureErr2943(
        "module IReproLib2943T2; "
        "public interface IHas2943T2 "
        "  property W: int get; set; end; "
        "end IHas2943T2; "
        "public class GIface2943T2<T>: IHas2943T2 "
        "  public _t: T; "
        "public _t2: T; "
        "public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GIface2943T2;",
        "module IReproApp2943T2; import IReproLib2943T2; "
        "public class M2943T2 "
        "  public static method Main(): int "
        "    using var g := new GIface2943T2<string>(); "
        "    var r: IHas2943T2 := g; "
        "    r.W := 7; "
        "    return 0; "
        "  end Main; "
        "end M2943T2;",
        "t2");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2943(r.err, "W", "IHas2943T2");
}

// ===========================================================================
// T3 — CONTROL — the layout-IDENTICAL case must NOT be declined. `GSame<T>`
// has NO T-typed storage, so the codegen-side `importedLayoutMoves` is
// false at every instantiation, and the itable slot IS filled. Today the
// property access returns the correct value (121) — the narrow predicate
// MUST keep this case in the accepted set, not over-reject.
// §13 / §23 / §2.5, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           layout_identical_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2943ct3_" + std::to_string(std::rand());
    TempFile2943Decline aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943Decline aObj((dir / (stem + "_a.o")).string());
    writeSource2943Decline(aSrc.path,
        "module CtlLib2943T3; "
        "public interface ICtl2943T3 "
        "  property W: int get; end; "
        "end ICtl2943T3; "
        "public class GCtl2943T3<T>: ICtl2943T3 "
        "  public _w: int := 21; "
        "  public property W: int get => _w + 100; end; "
        "end GCtl2943T3;");
    if (!compileToObj2943Decline(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2943Decline bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943Decline bExe((dir / stem).string());
    writeSource2943Decline(bSrc.path,
        "module CtlApp2943T3; import CtlLib2943T3; "
        "public class MCtl2943T3 "
        "  public static method Main(): int "
        "    using var g := new GCtl2943T3<string>(); "
        "    var r: ICtl2943T3 := g; "
        "    return r.W; "
        "  end Main; "
        "end MCtl2943T3;");
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
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 121);  // 21 + 100
}

// ===========================================================================
// T4 — non-generic interface on a non-generic imported class. There is
// NO layout-movability and the property access works — must NOT be
// declined. Pins the predicate's "(b) generic class implementor" arm:
// without it, every interface property on every imported class would
// over-reject.
// §13 / §23, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           non_generic_implementor_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2943ct4_" + std::to_string(std::rand());
    TempFile2943Decline aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943Decline aObj((dir / (stem + "_a.o")).string());
    writeSource2943Decline(aSrc.path,
        "module CtlLib2943T4; "
        "public interface ICtl2943T4 "
        "  property W: int get; end; "
        "end ICtl2943T4; "
        "public class Ctl2943T4: ICtl2943T4 "
        "  public _w: int := 21; "
        "  public property W: int get => _w + 100; end; "
        "end Ctl2943T4;");
    if (!compileToObj2943Decline(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2943Decline bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943Decline bExe((dir / stem).string());
    writeSource2943Decline(bSrc.path,
        "module CtlApp2943T4; import CtlLib2943T4; "
        "public class MCtl2943T4 "
        "  public static method Main(): int "
        "    using var c := new Ctl2943T4(); "
        "    var r: ICtl2943T4 := c; "
        "    return r.W; "
        "  end Main; "
        "end MCtl2943T4;");
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        if (!driver.compileFile(bSrc.path, bExe.path, deps).success) {
            BEZEL_EXPECT_TRUE(false);
            return;
        }
    }
    int status = std::system(bExe.path.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 121);
}

// ===========================================================================
// T5 — the same generic class with one T-typed field is the codegen-side
// layout-IDENTICAL maintenance-note shape (#2906 / #2943's own
// maintenance notes): with a SINGLE T-typed field the i32 placeholder
// and the `ptr` share one 8-byte-aligned slot, and `_w`'s offset matches
// the erased template at every T. The codegen-side
// `instanceLayoutDiffersFromErasedTemplate` says "no move" for this
// shape, so the codegen FILLS the slot and the program returns the
// correct value 121 today.
//
// The sema-side predicate (d) ("the implementor has T-typed fields")
// DOES fire for `GCtl5<T>` — it has one, after all — so the decline fires
// in the OVER-rejection direction documented at the predicate's
// definition. This test pins BOTH outcomes as a RANGE the way the
// codegen-side test does for the SEGV-vs-correct-value pair: a future
// sema-side refinement that adds a more precise layout-move check
// (counting T-typed fields, or simulating the layout) would naturally
// collapse this case to `success` and 121; today both are valid.
// §14.1 / §23, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           single_t_field_over_rejection_range_spec_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2943ct5_" + std::to_string(std::rand());
    TempFile2943Decline aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943Decline aObj((dir / (stem + "_a.o")).string());
    writeSource2943Decline(aSrc.path,
        "module OvrLib2943T5; "
        "public interface IOvr2943T5 "
        "  property W: int get; end; "
        "end IOvr2943T5; "
        "public class GCtl2943T5<T>: IOvr2943T5 "
        "  public _t: T; "      // SINGLE T-typed field (the maintenance-note shape)
        "  public _w: int := 21; "
        "  public property W: int get => _w + 100; end; "
        "end GCtl2943T5;");
    if (!compileToObj2943Decline(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2943Decline bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943Decline bExe((dir / stem).string());
    writeSource2943Decline(bSrc.path,
        "module OvrApp2943T5; import OvrLib2943T5; "
        "public class MOvr2943T5 "
        "  public static method Main(): int "
        "    using var g := new GCtl2943T5<string>(); "
        "    var r: IOvr2943T5 := g; "
        "    return r.W; "
        "  end Main; "
        "end MOvr2943T5;");

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

    if (success) {
        int status = std::system(bExe.path.c_str());
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
        std::remove(bExe.path.c_str());
        BEZEL_EXPECT_EQ(code, 121);  // codegen-side correct path
    } else {
        std::remove(bExe.path.c_str());
        expectDecline2943(err, "W", "IOvr2943T5");  // sema-side over-rejection
    }
}

}  // namespace bezel::module
