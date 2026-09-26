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
// (`ModuleLoader.cpp`, `parseAndAttachMethodBody`): admit a member call the
// METHOD's own bound provides into the body-symbol remap, exactly as #4553
// does for a class-header bound, so `v.Area()` binds to the imported
// interface member and codegen monomorphizes the call per instantiation.
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

}  // namespace bezel::module
