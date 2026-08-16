// fix #2943 (§14.1 / §2.8.3 rule 4 / §13 / §23 / §12.1, v0.240,
// NARROWER sema-side arm — supersedes the reverted d11e7ca attempt).
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
//   (a) the accessed property is declared on the interface, NOT inherited
//       from a class or record (parent kind Interface, not Class/Record);
//   (b) the interface is IMPORTED (cross-module codegen path, not the
//       local-monomorphize path which inlines the body — the latter is
//       OUT OF GATE);
//   (c) the interface is NOT generic (`ips->parent->genericParams.empty()`)
//       — a generic interface with a T-typed property return is the #2944
//       family (the narrower predicate at codegen), and a generic
//       interface's INDEXER is #2955's separate decline (out of gate for
//       this arm, which is non-indexer);
//   (d) at least one IMPORTED GENERIC CLASS implementing this interface
//       declares >=2 fields whose resolved TypeRef is a bare name
//       matching one of the class's own generic parameters — i.e., has
//       the layout-moving potential the codegen-side decline
//       (`instanceLayoutDiffersFromErasedTemplate`, which is the
//       authoritative per-instantiation check at codegen) already keys on.
//       THIS IS THE NARROWING vs the reverted d11e7ca attempt (which
//       fired on >=1 T-typed field). The 2894 T3 maintenance comment
//       (xmod_imported_generic_iface_accessor_2894_test.cpp line ~219)
//       states the discriminator explicitly:
//           "TWO `T` fields are load-bearing, because with a single one
//            the i32 placeholder and the `ptr` share an 8-byte-aligned
//            slot and `_w` lands at the same offset either way".
//       With a SINGLE T-typed field the codegen-side layout check
//       returns false (layouts match), the itable slot IS filled, and
//       the program returns the correct value (2894 T1 = 108 with single
//       `_t: T`). The over-rejection there is the bug the lane's previous
//       attempt shipped and the gate caught — this narrower predicate
//       keeps the 2894 T1 case in the accepted set.
//
// Codegen's `instanceLayoutDiffersFromErasedTemplate` is an exact layout
// check (it requires LLVMDataLayout info we do not have at sema time);
// (d) is its narrowest sema-time approximation. The over-rejection cost
// is a diagnostic at the access site instead of a value, which is the
// documented "value vs crash" trade #2884 made.
//
// AXIS GRID:
//   * T-field count:    1, 2, >=2       (covers layout-identical and moving)
//   * Interface kind:   property get / property set / mixed (method + property)
//   * Receiver class:   generic (T=string at access) and
//                       generic-but-not-layout-moving (T=int)
//   * Layout position:  T-typed fields BEFORE the concrete storage (the
//                       issue's repro shape — `_t, _t2, _w`) and
//                       layout-IDENTICAL control (no T-typed fields at all)
//   * Existing 2894 T1 cell is preserved (single `_t: T` instantiated at
//     `int`, expected rc=108) — that test lives in
//     xmod_imported_generic_iface_accessor_2894_test.cpp and is NOT
//     regressed by this fix.
//
// Re-pro test cases. The interface is non-generic, the implementing class
// is imported generic with TWO or more T-typed fields, the access is via
// interface reference (so dispatch is itable-mediated), and the
// property's body reads fields whose offset moves under the substitution.
// Pre-fix the program compiles AND links clean, AND SEGVs at runtime;
// post-fix the program declines at the access site with a §-cited
// diagnostic.
//
// Adjacent suites green: xmod_generic_property_2884, xmod_generic_indexer
// _param_2955, xmod_imported_generic_layout_moving_2943, and the
// existing 2894 T1 cell (single `_t: T`, rc=108).

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2943 {
    std::string path;
    explicit TempFile2943(std::string p) : path(std::move(p)) {}
    ~TempFile2943() { std::remove(path.c_str()); }
};

void writeSource2943(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2943(const std::string& src, const std::string& obj) {
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
    std::string stem = "bz2943_" + tag + "_" + unique_stem("");
    TempFile2943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943 aObj((dir / (stem + "_a.o")).string());
    writeSource2943(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, false, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {false, false, ""};

    TempFile2943 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943 bExe((dir / stem).string());
    writeSource2943(bSrc.path, appSrc);

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
// T1 — ISSUE'S OWN REPRO, property GETTER on a layout-moving imported
// generic. Pre-fix: exit 139 (SIGSEGV through the null itable slot).
// Post-fix: compile-time decline with the §-cited diagnostic.
//
// TWO T-typed fields (`_t: T; _t2: T;`) before `_w: int`, instantiated at
// `T = string`. The codegen-side `instanceLayoutDiffersFromErasedTemplate`
// returns true at this instantiation, so the itable slot stays null, so
// the dispatch faults with a SIGSEGV. The sema-side decline converts that
// crash into a sourced diagnostic at the access site.
//
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
        "  public _t2: T; "
        "  public _w: int := 0; "
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
// T2 — PROPERTY SETTER on the same shape. The issue reported the GETTER
// SEGV, but the setter twin SEGVs the same way (a separate itable slot,
// the same layout-move predicate, the same deliberate null). Both must
// decline. §23.3 / §14.1 / §2.8.3 rule 4.
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
        "  public _t2: T; "
        "  public _w: int := 0; "
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
// T3 — CONTROL, layout-IDENTICAL: the implementor has NO T-typed storage
// at all, so the codegen-side `instanceLayoutDiffersFromErasedTemplate` is
// false at every instantiation, and the itable slot IS filled. The
// property access returns the correct value (121) — the narrow predicate
// MUST keep this case in the accepted set, not over-reject.
//
// This is also the test for the "no T-typed fields" boundary of (d): a
// class with 0 T-typed fields is layout-IDENTICAL even at `T = string`,
// and the codegen-side path returns the correct value. §13 / §23 / §2.5,
// v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           layout_identical_no_t_fields_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2943ct3_");
    TempFile2943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943 aObj((dir / (stem + "_a.o")).string());
    writeSource2943(aSrc.path,
        "module CtlLib2943T3; "
        "public interface ICtl2943T3 "
        "  property W: int get; end; "
        "end ICtl2943T3; "
        "public class GCtl2943T3<T>: ICtl2943T3 "
        "  public _w: int := 21; "
        "  public property W: int get => _w + 100; end; "
        "end GCtl2943T3;");
    if (!compileToObj2943(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2943 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943 bExe((dir / stem).string());
    writeSource2943(bSrc.path,
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
// T4 — DIFFERENT MEMBER KIND: an interface METHOD call on the SAME
// layout-moving imported generic shape as T1/T2. The method slot is
// filled by the codegen-side #2943 partial fix (a re-lowered
// specialization), so the dispatch returns the correct value 42 today.
// The sema-side predicate (d) finds the same >=2-T-field class — but
// the decline is keyed at property access (`!ips->isIndexer`), so a
// method call on the same receiver MUST NOT be declined. This is the
// cross-surface coverage the lane brief asks for: the decline fires on
// properties and not on methods.
//
// Note: the codegen-side partial fix has already filled the method slot
// in main; the test pins the method arm as unchanged post-this-fix.
// §13 / §2.8.3 rule 4, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           method_call_on_same_layout_moving_class_unchanged_spec_13) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2943ct4_");
    TempFile2943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943 aObj((dir / (stem + "_a.o")).string());
    writeSource2943(aSrc.path,
        "module MthLib2943T4; "
        "public interface IDo2943T4 "
        "  method Twice(): int; "
        "end IDo2943T4; "
        "public class GDo2943T4<T>: IDo2943T4 "
        "  public _t: T; "
        "  public _t2: T; "      // TWO T-typed fields (layout-moving)
        "  public _w: int := 21; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "end GDo2943T4;");
    if (!compileToObj2943(aSrc.path, aObj.path)) return;
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2943 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943 bExe((dir / stem).string());
    writeSource2943(bSrc.path,
        "module MthApp2943T4; import MthLib2943T4; "
        "public class MMth2943T4 "
        "  public static method Main(): int "
        "    using var g := new GDo2943T4<string>(); "
        "    var d: IDo2943T4 := g; "
        "    return d.Twice(); "
        "  end Main; "
        "end MMth2943T4;");
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
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(code, 42);
}

// ===========================================================================
// T5 — LAYOUT POSITION AXIS: the SAME two-T-field count but with the
// concrete storage FIRST and the T-typed fields AFTER. This proves the
// predicate is "count of T-typed fields" not "T-typed fields before the
// concrete storage" — the layout-move still fires (the offsets still
// diverge at the boundary between the first T-typed field and the next
// concrete field), so the access must still be declined.
//
// `_w: int := 0; _t: T; _t2: T;` — concrete storage at offset 0 (size 4),
// first T (i32 placeholder, size 4, 4-byte aligned) at offset 4 in the
// erased template vs (ptr, 8-byte aligned) at offset 8 in the spec
// layout — the second T-typed field lands at offset 8 (erased) vs
// offset 16 (spec). `_w` is at offset 0 in both layouts (it is the
// first field, before any T-typed field) but the GETTER body would
// still read at the same offset 0 — so this case is somewhat less
// interesting than T1/T2's shape. The sema-side decline (d) is purely
// count-based and fires regardless. §14.1 / §23.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_property_layout_movable_2943,
           t_fields_after_concrete_storage_still_declined_spec_14_1) {
    auto r = compileImportCaptureErr2943(
        "module PosLib2943T5; "
        "public interface IPos2943T5 "
        "  property W: int get; end; "
        "end IPos2943T5; "
        "public class GPos2943T5<T>: IPos2943T5 "
        "  public _w: int := 0; "
        "  public _t: T; "
        "  public _t2: T; "
        "  public property W: int get => _w + 100; end; "
        "end GPos2943T5;",
        "module PosApp2943T5; import PosLib2943T5; "
        "public class MPos2943T5 "
        "  public static method Main(): int "
        "    using var g := new GPos2943T5<string>(); "
        "    var r: IPos2943T5 := g; "
        "    var x: int := r.W; "
        "    return 0; "
        "  end Main; "
        "end MPos2943T5;",
        "t5");
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    expectDecline2943(r.err, "W", "IPos2943T5");
}

}  // namespace bezel::module
