// Spec-derived regression test for issue #4600 — an imported generic method
// with a RETURN-ONLY type parameter silently returns null cross-module.
//
// spec §14.5 "Type-argument inference for generic methods" (phase 3,
// "Target-typed completion") / §14.1 "Generic methods" / §2.5 "Modules" /
// §2.8.3 rule 2 (an imported artifact must not alter an instantiation's
// semantics), v0.307.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §2.8.3 rule 2: calling a method across a module boundary must behave exactly
// as calling the byte-identical declaration in the same module. §14.1 makes a
// generic method's body monomorphized per instantiation; nothing in §14.5 or
// §2.5 makes the module boundary a point at which an instantiation degrades.
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// A generic method's body is compiled ONCE in its defining module, against the
// i32 type-parameter placeholder. For a generic CLASS (#783) and for a
// non-generic class's record-returning factory (#3645) the exporter ships the
// body text (`MetaMember::methodBodyText`), the importer re-parses it, and
// codegen monomorphizes per instantiation. For a GENERIC METHOD on a
// non-generic class the exporter shipped nothing — the #3645 scope-narrow
// parked that as the broader #1937/#428 work item — so the importer calls the
// defining module's placeholder template directly.
//
// Two gates fail that call closed where it is unsound. Neither covers this
// shape:
//   * #1937 (sema, ExprTypecheck_Calls.cpp) fires when the declared RETURN
//     carries a generic RECORD parameterized by a method type parameter. Its
//     own scope note leaves a bare-`T` return alone, reasoning that
//     `Echo<T>(x: T): T` "round-trips for a placeholder-matching
//     instantiation" — true for a type parameter an ARGUMENT hands through.
//   * #1943 (codegen, LlvmBackend_Calls2.cpp) fires when a type-param-typed
//     ARGUMENT reaches the i32 placeholder parameter at a different width.
//
// A RETURN-ONLY type parameter (§14.5 phase 3: it appears in no parameter, so
// only the call's target can bind it) has NO argument. #1943's loop has
// nothing to compare, #1937's walk finds no record, and the call is emitted
// against a template whose return is the i32 placeholder while the caller's
// slot is whatever the instantiation is. The exporter even prints the cause
// while compiling the dependency — `new T()` in the dead abstract template
// body (#2477), whose return #2491 degrades to null.
//
// ── MEASURED, on 27e2cc5e, with real two-module builds ─────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   `Factory.Create<T>(): owned T where T: class, new()` called as
//   `using var w: Widget := Factory.Create(); return Widget._n;`
//
//               | same module | cross-module
//   diagnostics |    none     |    none
//   exit code   |      1      |     0        <- the constructor never ran
//
//   and under generated-code AddressSanitizer the cross-module program is
//   `stack-buffer-overflow`, READ of size 8 at a 4-byte stack object in main.
//
// ⚠ ONE CORRECTION TO THE ISSUE, measured here. The issue's follow-up comment
// records the EXPLICIT-type-argument twin (`Create<Widget>()`) as ASan-CLEAN
// and concludes "the discriminator is precisely that the type argument was
// solved by §14.5 phase 3 rather than spelled out". That was true when it was
// written and is NOT true on this base: #4622 (landed since) makes the
// explicit path substitute the call's return type too, so the caller's slot
// became pointer-sized there as well and the EXACT program from that comment
// now overflows identically. T3 pins that half, which is why the fix had to
// cover both spellings rather than only phase 3.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Export side only (`ModuleMetadataBuilder.cpp`): ship `methodBodyText` for a
// generic method whose declared return type mentions a type parameter that NO
// formal parameter mentions. `ModuleLoader`'s re-parse already admits any
// class carrying a non-empty `methodBodyText` (its own #3645 widening), and
// codegen already monomorphizes a call whose imported symbol has a live
// `ast::MethodDecl`, so nothing else had to change and no wire-format field
// was added.
//
// The recognizer is deliberately per-method and per-shape: a type parameter
// mentioned in ANY parameter is excluded, which is what keeps #1937 and #1943
// in force (T5/T6 below). Widening it to every generic method was tried and
// MEASURED: both of those shapes then newly compile, which is the broader
// #1937/#428 monomorphization work item and not this fix.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ THE BUG. Imported return-only `<T>` with a target. Asserts the
//        constructor ran exactly once (exit 1). Pre-fix: exit 0, no
//        diagnostics, plus an ASan stack-buffer-overflow under `--full`.
//   T2   DIFFERENTIAL CONTROL. The byte-identical declarations and call in ONE
//        module — exit 1 before AND after, so the module boundary is proved to
//        be the whole difference T1 measures.
//   T3 ★ THE EXPLICIT-LIST HALF (see the correction above). Pre-fix: exit 0.
//   T4   The return-only parameter inside a constructed generic
//        (`MakeBoxed<T>(): owned BoxS<T>`) rather than bare — the recognizer
//        must walk generic arguments, and the re-parsed body must resolve a
//        SIBLING generic class. Exact value 4 = the box's field PLUS the
//        constructor count, because this return type is a class and so the box
//        alone arrives intact pre-fix (measured 3). See the row.
//   T5   NO-REGRESSION, the expensive direction. #1937's record-return shape
//        in the SAME dependency must still be REJECTED with its SPECIFIC
//        diagnostic — the narrowing is per-method, not per-module.
//   T6   NO-REGRESSION. #1943's argument-width shape in the same dependency
//        must still be REJECTED with its SPECIFIC diagnostic.
//   T7   NO-REGRESSION. `Echo4600<T>(x: T): T` — argument-bound, nothing
//        ships, still runs. Exact value 42.
//   T8   NO-REGRESSION. The return-only call with NO target is still the
//        §14.5 phase 3 compile error (#4590/#4611); making the body available
//        must not turn that reject into a silent accept.
//
// Every reject row matches a SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure and would be vacuous here. Every accept row asserts
// an exact RUN value rather than "compiled", because "compiled" is precisely
// what the pre-fix compiler also did.

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

struct TempFile4600 {
    std::string path;
    explicit TempFile4600(std::string p) : path(std::move(p)) {}
    ~TempFile4600() { std::remove(path.c_str()); }
};

void writeSource4600(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled4600 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_imported_symbol_residues_3147_test.cpp established).
template <typename Fn>
std::string captureStderr4600(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz4600_err_" + unique_stem("") + ".txt")).string();
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

// Build `libSrc` to an object, then compile `appSrc` against it.
Compiled4600 compileImport4600(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4600_");
    TempFile4600 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4600 aObj((dir / (stem + "_a.o")).string());
    writeSource4600(aSrc.path, libSrc);
    Compiled4600 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile4600 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4600 bExe((dir / stem).string());
    writeSource4600(bSrc.path, appSrc);
    r.err = captureStderr4600([&] {
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
//
// `compileFileForTest` / `runProgramForTest` (#4532) are what wire this
// hand-rolled runner into `gate.sh --full`: with
// BEZEL_AOT_FORCE_SANITIZE=address the importer's code is built under
// generated-code ASan and run with the sweep's own options, so the
// stack-buffer-overflow this fix removes fails the row loudly while the row
// keeps asserting the VALUE it exists to assert.
int buildImportAndRun4600(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4600r_");
    TempFile4600 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4600 libObjF((dir / (id + "_lib.o")).string());
    TempFile4600 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile4600 mainExeF((dir / id).string());
    writeSource4600(libSrcF.path, libSrc);
    writeSource4600(mainSrcF.path, mainSrc);

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

// The one-module twin of `buildImportAndRun4600` — no dependency anywhere.
int buildLocalAndRun4600(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4600l_");
    TempFile4600 srcF((dir / (id + ".bzl")).string());
    TempFile4600 exeF((dir / id).string());
    writeSource4600(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. ONE module carrying every shape the rows exercise, which is
// what makes T5/T6 load-bearing: the fix's recognizer runs per METHOD, so the
// shapes that must stay gated sit beside the shapes that must now work.
// ---------------------------------------------------------------------------
//
//   F4600.Create4600<T>(): owned T            RETURN-ONLY, bare      (T1,T2,T3,T8)
//   F4600.MakeBoxed4600<T>(): owned BoxS4600<T>  RETURN-ONLY, nested   (T4)
//   F4600.Make4600<A,B>(a,b): PairG4600<A,B>  arg-bound record  -> #1937 (T5)
//   F4600.Count4600<T>(x: T): int             arg-bound, no T in return
//                                                             -> #1943 (T6)
//   F4600.Echo4600<T>(x: T): T                arg-bound, bare        (T7)
const char* kDeclsBody4600 =
    "public record PairG4600<A, B>(F: A, S: B); "
    "public class BoxS4600<T> "
    "  public readonly property V: int; "
    "  public constructor() V := 3; end constructor; "
    "  destructor end destructor; "
    "end BoxS4600; "
    "public class Widget4600 "
    "  public static _n: int := 0; "
    "  public constructor() _n := _n + 1; end constructor; "
    "  destructor end destructor; "
    "end Widget4600; "
    "public static class F4600 "
    "  public static method Create4600<T>(): owned T where T: class, new() "
    "    return new T(); "
    "  end Create4600; "
    "  public static method MakeBoxed4600<T>(): owned BoxS4600<T> "
    "      where T: class, new() "
    "    using var inner := new T(); "
    "    return new BoxS4600<T>(); "
    "  end MakeBoxed4600; "
    "  public static method Echo4600<T>(x: T): T "
    "    return x; "
    "  end Echo4600; "
    "  public static method Count4600<T>(x: T): int where T: value "
    "    return 7; "
    "  end Count4600; "
    "  public static method Make4600<A, B>(a: A, b: B): PairG4600<A, B> "
    "    where A: value where B: value "
    "    return new PairG4600<A, B>(a, b); "
    "  end Make4600; "
    "end F4600; ";

std::string dep4600() {
    return std::string("module Dep4600; ") + kDeclsBody4600;
}

// The importing module, parameterised by the body of `Main`.
std::string app4600(const std::string& tag, const std::string& mainBody) {
    return "module App4600" + tag + "; import Dep4600; " +
           "class C4600 public static method Main(): int " + mainBody +
           " end Main; end C4600;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local4600(const std::string& tag, const std::string& mainBody) {
    return "module Loc4600" + tag + "; " + kDeclsBody4600 +
           "class C4600 public static method Main(): int " + mainBody +
           " end Main; end C4600;";
}

bool mentions4600(const Compiled4600& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// ★ THE OBSERVABLE. `_n` counts constructor entries, so the exit code answers
// "did the constructor run?" rather than "did something compile" — the
// pre-fix binary compiles clean and answers 0.
const char* kCtorRanTargetBody4600 =
    "using var w: Widget4600 := F4600.Create4600(); return Widget4600._n;";
const char* kCtorRanExplicitBody4600 =
    "using var w := F4600.Create4600<Widget4600>(); return Widget4600._n;";

// §14.1/§2.8.3, as the #1937 sema gate emits it.
const char* kRecordReturnReject4600 =
    "calling imported generic method `Make4600`, which returns a generic "
    "record parameterized by its own type parameter(s)";
// §14.1/§2.8.3, as the #1943 codegen gate emits it.
const char* kArgWidthReject4600 =
    "calling imported generic method `Count4600` with a type argument whose "
    "representation is not the compiled `int` (i32) placeholder";
// §14.1/§A.7, as `reportNoTargetInferenceIfNeeded` emits it.
const char* kNoTargetHead4600 =
    "cannot infer the type argument of `Create4600`'s call";
const char* kNoTargetTail4600 = "a target type is required";

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG. An imported generic method whose type parameter appears only
// in the return type, bound by §14.5 phase 3 from the typed local. The
// constructor must run exactly once, as it does for the byte-identical
// single-module program (T2). Pre-fix: exit 0 with zero diagnostics — the
// importer called the defining module's i32-placeholder template, whose `new
// T()` body #2477 could not lower and #2491 degraded to a null return.
// // per §14.5 phase 3 / §14.1 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           imported_return_only_type_param_runs_the_constructor) {
    int rc = buildImportAndRun4600(dep4600(),
                                   app4600("T1", kCtorRanTargetBody4600));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 1);
}

// ===========================================================================
// T2 — DIFFERENTIAL CONTROL for T1. The byte-identical declarations and call
// in ONE module: exit 1 before AND after this fix, which is what makes T1's
// exit 0 attributable to the module boundary and nothing else (§2.8.3 rule 2).
// // per §14.5 phase 3 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           same_module_twin_runs_the_constructor_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4600(
                        local4600("T2", kCtorRanTargetBody4600)),
                    1);
}

// ===========================================================================
// T3 — ★ THE EXPLICIT-LIST HALF. The same return-only method with the type
// argument SPELLED OUT. #4600's follow-up comment recorded this spelling as
// ASan-clean and concluded the phase-3 solve was the discriminator; that
// stopped being true when #4622 landed (it makes the explicit path substitute
// the call's return type, so the caller's slot became pointer-sized here too)
// and the exact program from that comment now overflows identically. Both
// spellings therefore had to be covered, and this row is what would catch a
// fix that only handled phase 3. Pre-fix: exit 0.
// // per §14.5 "Explicit lists are all-or-nothing" / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           explicit_type_argument_spelling_runs_the_constructor) {
    int rc = buildImportAndRun4600(dep4600(),
                                   app4600("T3", kCtorRanExplicitBody4600));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// ===========================================================================
// T4 — the return-only parameter NESTED inside a constructed generic
// (`MakeBoxed4600<T>(): owned BoxS4600<T>`) rather than appearing bare. Two
// distinct things have to hold that T1 does not test: the exporter's
// recognizer must walk into generic ARGUMENTS to see `T` at all, and the body
// the importer re-parses must resolve a SIBLING generic class (`new
// BoxS4600<T>()`), not just the type parameter.
//
// The observable is `3 + _n`, both halves deliberately. This return type is a
// CLASS, so the placeholder template already returns a pointer and the ABI
// matches — the box itself arrives intact even pre-fix. MEASURED pre-fix: 3,
// i.e. `b.V` is right and `_n` is 0, because the `new T()` INSIDE the body
// never ran. A row asserting only `b.V` would therefore have passed on both
// sides of this fix and pinned nothing; this one separates "the call returned
// something" from "the body was monomorphized", which is the actual claim.
// // per §14.5 phase 3 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           return_only_param_nested_in_generic_argument_runs_the_body) {
    int rc = buildImportAndRun4600(
        dep4600(),
        app4600("T4", "using var b: BoxS4600<Widget4600> := "
                      "F4600.MakeBoxed4600(); var v := b.V; "
                      "return v + Widget4600._n;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// T5 — NO-REGRESSION, and the expensive direction. `Make4600<A, B>(a: A, b: B)
// : PairG4600<A, B>` lives in the SAME dependency module as the methods T1-T4
// fix. Both of its type parameters are argument-bound, so nothing ships for
// it and the #1937 sema gate must still reject the call with its SPECIFIC
// diagnostic. This is the row that fails if the export gate is widened from
// "return-only" to "generic method": MEASURED — with that widening this shape
// newly COMPILES (and #1943's below runs, returning 7), which is the broader
// #1937/#428 monomorphization work item, not this fix.
// // per §14.1 / §2.8.3 / #1937, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           sibling_record_return_method_still_rejected) {
    auto r = compileImport4600(
        dep4600(),
        app4600("T5", "var p := F4600.Make4600<long, long>(10, 20); "
                      "var s: long := p.F + p.S; "
                      "if s = 30 then return 1; end if; return 0;"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4600(r, kRecordReturnReject4600));
}

// ===========================================================================
// T6 — NO-REGRESSION. `Count4600<T>(x: T): int` — its return mentions no type
// parameter, so nothing ships for it either and the #1943 codegen
// argument-width gate must still reject a non-i32 instantiation with its
// SPECIFIC diagnostic. Same widening argument as T5.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           sibling_argument_width_method_still_rejected) {
    auto r = compileImport4600(
        dep4600(),
        app4600("T6", "var n := F4600.Count4600<double>(3.0); return n;"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4600(r, kArgWidthReject4600));
}

// ===========================================================================
// T7 — NO-REGRESSION / ANTI-FALSE-REJECT. `Echo4600<T>(x: T): T` binds T from
// its ARGUMENT (§14.5 phase 1), so the recognizer must NOT treat it as
// return-only; it keeps the pre-fix path and must keep its exact value.
// // per §14.5 phase 1 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           argument_bound_type_param_unchanged) {
    int rc = buildImportAndRun4600(
        dep4600(),
        app4600("T7", "var x: int := F4600.Echo4600(41); return x + 1;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T8 — NO-REGRESSION. §14.5 phase 3: a return-only type parameter with NO
// target is "a compile error naming the parameter" (#4590/#4611 made that fire
// across a module boundary). Making the body available to the importer must
// not turn that reject into a silent accept — the diagnostic is about
// INFERENCE, and inference still has nothing to work from.
// // per §14.5 phase 3 / §A.7, v0.307
// ===========================================================================
BEZEL_TEST(xmod_return_only_generic_method_4600,
           no_target_call_still_rejected) {
    auto r = compileImport4600(
        dep4600(),
        app4600("T8", "using var w := F4600.Create4600(); return 9;"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4600(r, kNoTargetHead4600));
    BEZEL_EXPECT_TRUE(mentions4600(r, kNoTargetTail4600));
}

}  // namespace bezel::module
