// Spec-derived regression test for issue #5009 — an imported generic method
// whose body calls a member its `where T: IFace` bound provides was compiled
// only as the defining module's i32-placeholder template, so every call that
// reached it returned a silent zero.
//
// spec §14.1 "Generic methods" (a generic method's body is monomorphized per
// instantiation) / §14.2 "Generic constraints" (a type bound admits calls to
// the bound's members on a type-parameter-typed value; `new()` admits
// `new T()`) / §2.8.3 rule 2 (an imported artifact must not alter an
// instantiation's semantics), v0.307.
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// A generic method on a NON-generic class is compiled once in its defining
// module against the i32 type-parameter placeholder. `v.Area()` on a
// placeholder-typed `v` cannot be lowered there, so the template body degrades
// to a zero return with the INTERNAL #2491 note (`new T()` likewise, #2477).
// The same-module program never calls that template — it monomorphizes — but
// the exporter shipped no body for this shape (#4600 opened the body-shipping
// path for RETURN-ONLY type parameters only), so an importer had nothing to
// monomorphize and called the template:
//
//   * `Gen<T>(v: T): int where T: IShape` with a class / record / string-
//     carrying argument: the #1943 argument-width gate rejected it (the
//     argument is not i32), so the constraint-dispatch shape could not be
//     called across a module boundary at all;
//   * `Mk<T>(): int where T: IShape, new()` — no type-parameter-typed
//     argument, so #1943 had nothing to compare and the call ran the
//     template: exit 0, where the byte-identical single-module program
//     returns 7. SILENT WRONG CODE.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Export side (`ModuleMetadataBuilder.cpp`, `isConstraintUsingGenericMethod`):
// ship `methodBodyText` for a generic method whose own type parameter carries
// a `new()` bound or a TYPE bound declared in the same module. Import side
// (`ModuleLoader.cpp`, `parseAndAttachMethodBody`):
//   * a member of ANY public sibling type of the exporting module (and the
//     sibling type itself, as a qualifier) is carried into the body-symbol
//     remap by identity. That covers the bound's `v.Area()` and also
//     `Helper.Bonus()`, `b.V`, `b.Twice()` and `Consts.K`;
//   * on this route the reconstruction FAILS CLOSED. If any in-body reference
//     to the reconstructed type, one of its members, or a parameter is left
//     unbound, the body is not attached and the call keeps the #1943 clean
//     reject.
//
// The first version of this fix (539563d5) admitted only the bound's members.
// Every other sibling member was dropped, so programs that were previously
// rejected cleanly compiled and returned wrong values (T11-T13, found by the
// non-author review). T11-T16 pin that class.
//
// Deliberately NOT covered, and pinned as unchanged (T9): a bound on a
// BUILTIN interface (`INumber<T>`, `IComparable<T>`) — the importer's fresh
// re-parse context re-synthesizes those interfaces as distinct symbols, so
// shipping that body turned a working call into a spurious §14.2 reject.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ class argument, T inferred        -> 7   (pre-fix: #1943 reject)
//   T2 ★ class argument, explicit <Sq>     -> 7   (pre-fix: #1943 reject)
//   T3 ★ record (value kind) argument      -> 12  (pre-fix: #1943 reject)
//   T4 ★ string-carrying record argument   -> 5   (pre-fix: #1943 reject)
//   T5 ★ string-carrying class argument    -> 5   (pre-fix: #1943 reject)
//   T6 ★ interface-typed argument          -> 7   (pre-fix: #1943 reject)
//   T7 ★ THE SILENT ZERO. `Mk<Sq>()`, no T-typed argument -> 7 (pre-fix: 0)
//   T8   DIFFERENTIAL CONTROL: all of the above in ONE module -> 35
//   T9   NO-REGRESSION: builtin-bound `GenNum<int>` keeps the template -> 4
//   T10  NO-REGRESSION: #1943's `Count<T>(x: T): int where T: value` with a
//        `double` is still REJECTED with its specific diagnostic.
//   T11 ★ body calls a public sibling's static method, both inline and
//        through a local                -> 27 each (539563d5: 0 / 7)
//   T12 ★ body reads a sibling's field, calls its instance method, reads a
//        sibling's static readonly      -> 11+15+47 = 73 (539563d5: 0 0 0)
//   T13 ★ the importer's OWN conformer through T11's body -> 23 (539563d5: 3)
//   T14 ★ `new()`-only bodies calling a sibling's / the own class's static
//        method, with an importer-side homonym `HelperS5009` -> 27+37 = 64
//   T15 ★ a self-QUALIFIED static call `GS5009.PubHelp()` -> 37
//   T16 ★ FAIL CLOSED: a body naming a computed property the remap cannot
//        bind is REJECTED with #1943's diagnostic (without the fail-closed
//        check: exit 0 with INTERNAL #2491, measured)
//   T17   DIFFERENTIAL CONTROL for T11-T15 in ONE module -> 228
//
// Every accept row asserts the constraint method's exact VALUE — "compiled"
// is what the pre-fix T7 also did.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5009 {
    std::string path;
    explicit TempFile5009(std::string p) : path(std::move(p)) {}
    ~TempFile5009() { std::remove(path.c_str()); }
};

void writeSource5009(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled5009 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so the driver's diagnostics
// are captured (the shape xmod_return_only_generic_method_4600 uses).
template <typename Fn>
std::string captureStderr5009(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5009_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile (only) `appSrc` against it.
Compiled5009 compileImport5009(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5009_");
    TempFile5009 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5009 aObj((dir / (stem + "_a.o")).string());
    writeSource5009(aSrc.path, libSrc);
    Compiled5009 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile5009 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5009 bExe((dir / stem).string());
    writeSource5009(bSrc.path, appSrc);
    r.err = captureStderr5009([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
// `compileFileForTest` / `runProgramForTest` (#4532) wire this runner into
// `gate.sh --full`: under BEZEL_AOT_FORCE_SANITIZE=address the importer is
// built with generated-code ASan while the row keeps asserting its VALUE.
int buildImportAndRun5009(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5009r_");
    TempFile5009 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5009 libObjF((dir / (id + "_lib.o")).string());
    TempFile5009 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5009 mainExeF((dir / id).string());
    writeSource5009(libSrcF.path, libSrc);
    writeSource5009(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The one-module twin — no dependency anywhere.
int buildLocalAndRun5009(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5009l_");
    TempFile5009 srcF((dir / (id + ".bzl")).string());
    TempFile5009 exeF((dir / id).string());
    writeSource5009(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. The constraint interface, four conformers (a class, a
// record, a string-carrying record, a string-carrying class), the two
// constraint-using generic methods the fix ships, and two NO-REGRESSION
// siblings in the SAME module (the recognizer runs per METHOD):
//
//   G5009.Gen5009<T>(v: T): int where T: IShape5009        ships  (T1-T6,T8)
//   G5009.Mk5009<T>(): int where T: IShape5009, new()     ships  (T7,T8)
//   G5009.GenNum5009<T>(v: T): int where T: INumber<T>     builtin bound,
//                                                          template  (T9)
//   G5009.Count5009<T>(x: T): int where T: value          special-only,
//                                                          #1943    (T10)
// ---------------------------------------------------------------------------
const char* kDeclsBody5009 =
    "public interface IShape5009 "
    "  method Area(): int; "
    "end IShape5009; "
    "public class Sq5009: IShape5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq5009; "
    "public record Rect5009(W: int, H: int): IShape5009 "
    "  public method Area(): int => W * H; "
    "end Rect5009; "
    "public record Named5009(Name: string): IShape5009 "
    "  public method Area(): int => Name.Length; "
    "end Named5009; "
    "public class Tag5009: IShape5009 "
    "  public Label: string; "
    "  public constructor(s: string) Label := s; end constructor; "
    "  public method Area(): int return Label.Length + 1; end Area; "
    "end Tag5009; "
    "public static class G5009 "
    "  public static method Gen5009<T>(v: T): int where T: IShape5009 "
    "    return v.Area(); "
    "  end Gen5009; "
    "  public static method Mk5009<T>(): int where T: IShape5009, new() "
    "    using var t := new T(); "
    "    return t.Area(); "
    "  end Mk5009; "
    "  public static method GenNum5009<T>(v: T): int where T: INumber<T> "
    "    return 4; "
    "  end GenNum5009; "
    "  public static method Count5009<T>(x: T): int where T: value "
    "    return 7; "
    "  end Count5009; "
    "end G5009; ";

std::string dep5009() {
    return std::string("module Dep5009; ") + kDeclsBody5009;
}

std::string app5009(const std::string& tag, const std::string& mainBody) {
    return "module App5009" + tag + "; import Dep5009; " +
           "class C5009 public static method Main(): int " + mainBody +
           " end Main; end C5009;";
}

std::string local5009(const std::string& tag, const std::string& mainBody) {
    return "module Loc5009" + tag + "; " + kDeclsBody5009 +
           "class C5009 public static method Main(): int " + mainBody +
           " end Main; end C5009;";
}

// Every conformer through both shipped methods: 7 + 12 + 5 + 4 + 7 (Mk) =
// 35 (`Tag5009("abc")` is 3 + 1).
// The differential total T8 compares.
const char* kAllBody5009 =
    "using var s := new Sq5009(); "
    "var r := new Rect5009(3, 4); "
    "var n := new Named5009(\"hello\"); "
    "using var t := new Tag5009(\"abc\"); "
    "return G5009.Gen5009(s) + G5009.Gen5009(r) + G5009.Gen5009(n) + "
    "G5009.Gen5009(t) + G5009.Mk5009<Sq5009>();";

// §14.1/§2.8.3, as the #1943 codegen gate emits it.
const char* kArgWidthReject5009 =
    "calling imported generic method `Count5009` with a type argument whose "
    "representation is not the compiled `int` (i32) placeholder";

// ---------------------------------------------------------------------------
// The second dependency, for T11-T17. Its constraint-using generic bodies
// reach PUBLIC SIBLINGS of their own class: a static method, a field, an
// instance method, a static readonly field, and the own class qualified by
// name. `AccS5009.GenPS5009` reads a COMPUTED property, which the importer's
// remap cannot bind, so it is the fail-closed row (T16).
// ---------------------------------------------------------------------------
const char* kSibDecls5009 =
    "public interface IShapeS5009 "
    "  method Area(): int; "
    "end IShapeS5009; "
    "public class SqS5009: IShapeS5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqS5009; "
    "public static class HelperS5009 "
    "  public static method Bonus(): int return 20; end Bonus; "
    "end HelperS5009; "
    "public class BoxS5009 "
    "  public V: int; "
    "  public constructor() V := 4; end constructor; "
    "  public method Twice(): int return V * 2; end Twice; "
    "end BoxS5009; "
    "public static class ConstsS5009 "
    "  public static readonly K: int := 40; "
    "end ConstsS5009; "
    "public class AccS5009 "
    "  public constructor() end constructor; "
    "  public property P: int => 5; "
    "  public method GenPS5009<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + P; "
    "  end GenPS5009; "
    "end AccS5009; "
    "public static class GS5009 "
    "  public static method PubHelp(): int return 30; end PubHelp; "
    "  public static method Gen<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + HelperS5009.Bonus(); "
    "  end Gen; "
    "  public static method Gen2<T>(v: T): int where T: IShapeS5009 "
    "    var x := HelperS5009.Bonus(); "
    "    return v.Area() + x; "
    "  end Gen2; "
    "  public static method GBox<T>(v: T): int where T: IShapeS5009 "
    "    using var b := new BoxS5009(); "
    "    return v.Area() + b.V; "
    "  end GBox; "
    "  public static method GTwice<T>(v: T): int where T: IShapeS5009 "
    "    using var b := new BoxS5009(); "
    "    return v.Area() + b.Twice(); "
    "  end GTwice; "
    "  public static method GK<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + ConstsS5009.K; "
    "  end GK; "
    "  public static method QSelf<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + GS5009.PubHelp(); "
    "  end QSelf; "
    "  public static method Mk<T>(): int where T: IShapeS5009, new() "
    "    using var t := new T(); "
    "    return t.Area() + HelperS5009.Bonus(); "
    "  end Mk; "
    "  public static method MkSelf<T>(): int where T: IShapeS5009, new() "
    "    using var t := new T(); "
    "    return t.Area() + PubHelp(); "
    "  end MkSelf; "
    "end GS5009; ";

std::string depSib5009() {
    return std::string("module DepS5009; ") + kSibDecls5009;
}

std::string appSib5009(const std::string& tag, const std::string& decls,
                       const std::string& mainBody) {
    return "module AppS5009" + tag + "; import DepS5009; " + decls +
           "class CS5009 public static method Main(): int " + mainBody +
           " end Main; end CS5009;";
}

// Every accepted sibling-reaching body: 27 (Gen) + 27 (Gen2) + 11 (GBox) +
// 15 (GTwice) + 47 (GK) + 37 (QSelf) + 27 (Mk) + 37 (MkSelf) = 228.
const char* kSibAllBody5009 =
    "using var s := new SqS5009(); "
    "return GS5009.Gen(s) + GS5009.Gen2(s) + GS5009.GBox(s) + "
    "GS5009.GTwice(s) + GS5009.GK(s) + GS5009.QSelf(s) + "
    "GS5009.Mk<SqS5009>() + GS5009.MkSelf<SqS5009>();";

const char* kFailClosedReject5009 =
    "calling imported generic method `GenPS5009` with a type argument whose "
    "representation is not the compiled `int` (i32) placeholder";

}  // namespace

// ===========================================================================
// T1 — ★ a CLASS argument, T inferred from it (§14.5 phase 1). The body's
// `v.Area()` dispatches through the imported `IShape5009` bound. Pre-fix: the
// #1943 gate rejected the call (the pointer argument is not the i32
// placeholder), so the constraint-dispatch shape was unusable cross-module.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           class_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T1", "using var s := new Sq5009(); "
                      "return G5009.Gen5009(s);"));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T2 — ★ the same call with the type argument SPELLED OUT (§14.5 "explicit
// lists"), so the fix is not specific to the inference arm.
// // per §14.5 / §14.1 / §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           explicit_type_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T2", "using var s := new Sq5009(); "
                      "return G5009.Gen5009<Sq5009>(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T3 — ★ a RECORD (value-kind) argument: the monomorphized body must take the
// record by value at its real layout and dispatch `Area()` to the record's
// own member. 3 * 4 = 12.
// // per §14.1 / §14.2 / §9.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           record_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T3", "var r := new Rect5009(3, 4); "
                      "return G5009.Gen5009(r);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 12);
}

// ===========================================================================
// T4 — ★ a STRING-carrying record: the constraint member reads a string field
// (`Name.Length`), so a wrong layout or a dropped retain would show in the
// value (or under the gate's ASan leg). "hello".Length = 5.
// // per §14.1 / §14.2 / §9.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           string_carrying_record_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T4", "var n := new Named5009(\"hello\"); "
                      "return G5009.Gen5009(n);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T5 — ★ a STRING-carrying class. "abcd".Length + 1 = 5.
// // per §14.1 / §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           string_carrying_class_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T5", "using var t := new Tag5009(\"abcd\"); "
                      "return G5009.Gen5009(t);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T6 — ★ an INTERFACE-typed argument (T bound to `IShape5009` itself): the
// call dispatches through the interface's own slot at run time.
// // per §14.1 / §14.2 / §11.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           interface_typed_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T6", "var s: IShape5009 := new Sq5009(); "
                      "return G5009.Gen5009(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T7 — ★ THE SILENT ZERO. `Mk5009<T>(): int where T: IShape5009, new()` has
// no type-parameter-typed argument, so #1943's width loop has nothing to
// compare and the pre-fix call ran the placeholder template, whose `new T()`
// (#2477) and `t.Area()` (#2491) both degraded: exit 0 with no diagnostic in
// the importing compile. The single-module program returns 7.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           new_bound_without_typed_argument_returns_constraint_value) {
    int rc = buildImportAndRun5009(
        dep5009(), app5009("T7", "return G5009.Mk5009<Sq5009>();"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T8 — DIFFERENTIAL CONTROL. Every conformer through both methods, once
// across the boundary and once in ONE module: the two must agree, and both
// must equal the hand-computed 7 + 12 + 5 + 4 + 7 = 35 —
// so §2.8.3 rule 2 is checked directly, not just against a constant.
// // per §2.8.3 rule 2 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           cross_module_matches_same_module_twin) {
    BEZEL_EXPECT_EQ(buildLocalAndRun5009(local5009("T8L", kAllBody5009)), 35);
    int rc = buildImportAndRun5009(dep5009(), app5009("T8X", kAllBody5009));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 35);
}

// ===========================================================================
// T9 — NO-REGRESSION. A BUILTIN interface bound (`where T: INumber<T>`) is
// deliberately NOT shipped: the importer's re-parse context would resynthesize
// `INumber` as a different symbol and the §14.2 call-site check then rejects
// the conforming `int`. MEASURED while developing this fix: with the body
// shipped this row was rejected. The body does not use the bound, so the
// template path it keeps is correct: 4.
// // per §14.2 / stdlib §2.19, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           builtin_interface_bound_keeps_working_template_path) {
    int rc = buildImportAndRun5009(
        dep5009(), app5009("T9", "return G5009.GenNum5009<int>(5);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// T10 — NO-REGRESSION. A special-only bound (`where T: value`) grants no
// member access, so nothing ships and #1943's argument-width gate must still
// reject a `double` instantiation with its SPECIFIC diagnostic.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           special_only_bound_still_argument_width_rejected) {
    auto r = compileImport5009(
        dep5009(),
        app5009("T10", "var n := G5009.Count5009<double>(3.0); return n;"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kArgWidthReject5009) != std::string::npos);
}

// ===========================================================================
// T11 — ★ REVIEW REGRESSION q1d. The body calls a PUBLIC sibling's static
// method, inline and through a local. 539563d5 attached the body but dropped
// the `HelperS5009` / `Bonus` bindings, and printed `0` and `7` for a program
// dcb17d57 rejected cleanly. 7 + 20 = 27 for both.
// // per §14.1 / §2.8.3 rule 2 / rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_static_call_in_body_returns_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T11a", "", "using var s := new SqS5009(); "
                               "return GS5009.Gen(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 27);
    rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T11b", "", "using var s := new SqS5009(); "
                               "return GS5009.Gen2(s);"));
    BEZEL_EXPECT_EQ(rc, 27);
}

// ===========================================================================
// T12 — ★ REVIEW REGRESSION q1i. A sibling's instance field (`b.V`), instance
// method (`b.Twice()`) and static readonly field (`ConstsS5009.K`).
// 539563d5 printed `0 0 0`. 11 + 15 + 47 = 73. Every term is positive, so a
// dropped one cannot hide in the sum.
// // per §14.1 / §2.8.3 rule 2 / rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_field_method_and_static_field_in_body_return_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T12", "", "using var s := new SqS5009(); "
                              "return GS5009.GBox(s) + GS5009.GTwice(s) + "
                              "GS5009.GK(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 73);
}

// ===========================================================================
// T13 — ★ REVIEW REGRESSION q1f. The importer's OWN conformer (`MineS5009`,
// Area 3) through the sibling-calling body: 3 + 20 = 23. 539563d5: 3.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           importer_own_conformer_through_sibling_calling_body) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T13",
                   "internal class MineS5009: IShapeS5009 "
                   "  public constructor() end constructor; "
                   "  public method Area(): int return 3; end Area; "
                   "end MineS5009; ",
                   "using var m := new MineS5009(); return GS5009.Gen2(m);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 23);
}

// ===========================================================================
// T14 — ★ `new()`-only bodies (no T-typed argument, so #1943 cannot catch a
// wrong value) calling a sibling's static method and the own class's static
// method. The importer declares its own `HelperS5009` (Bonus 50), which must
// NOT capture the dependency's. 27 + 37 = 64.
// // per §14.1 / §14.2 / §2.8.3 rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           new_bound_bodies_calling_statics_ignore_importer_homonym) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T14",
                   "internal static class HelperS5009 "
                   "  public static method Bonus(): int return 50; end Bonus; "
                   "end HelperS5009; ",
                   "return GS5009.Mk<SqS5009>() + GS5009.MkSelf<SqS5009>();"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 64);
}

// ===========================================================================
// T15 — ★ a self-QUALIFIED static call (`GS5009.PubHelp()` inside `GS5009`).
// The qualifier names the synthetic self type, which is rebound to the
// imported one. 7 + 30 = 37.
// // per §14.1 / §2.8.3 rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           self_qualified_static_call_in_body_returns_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T15", "", "using var s := new SqS5009(); "
                              "return GS5009.QSelf(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 37);
}

// ===========================================================================
// T16 — ★ FAIL CLOSED. `GenPS5009` reads a COMPUTED property of its own
// class, and the importer's remap has no arm that binds it. The body must NOT
// be attached, so the call keeps #1943's clean reject. MEASURED with the
// fail-closed return disabled: the call compiled with an INTERNAL #2491
// warning and returned 0 instead of 12.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           unbindable_body_reference_fails_closed_to_width_reject) {
    auto r = compileImport5009(
        depSib5009(),
        appSib5009("T16", "", "using var s := new SqS5009(); "
                              "using var a := new AccS5009(); "
                              "return a.GenPS5009(s);"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kFailClosedReject5009) != std::string::npos);
}

// ===========================================================================
// T17 — DIFFERENTIAL CONTROL for T11-T15: the same bodies in ONE module and
// across the boundary must both answer 228.
// // per §2.8.3 rule 2 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_reaching_bodies_match_same_module_twin) {
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5009(std::string("module LocS5009; ") +
                             kSibDecls5009 +
                             "class CS5009 public static method Main(): int " +
                             kSibAllBody5009 + " end Main; end CS5009;"),
        228);
    int rc = buildImportAndRun5009(depSib5009(),
                                   appSib5009("T17", "", kSibAllBody5009));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 228);
}

}  // namespace bezel::module
