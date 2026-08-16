// fix #2658 (§22.4 "Inline `out` declarations" / §22.3 "Parameter modifiers" /
// §2.5 "Dependency resolution"; v0.236) — `new ImportedT(…, out _)` emitted NO
// constructor call and NO diagnostic whatsoever.
//
// (Revision note: the checkout these tests were written against carries v0.237
// in its header; the v0.236->v0.237 diff to docs/Bezel_Language_Spec.md is the
// version line only, so the §22.3 / §22.4 text quoted below is byte-identical
// at both revisions and the v0.236 citation the sibling #2629/#2639/#2657
// commits use stays exact.)
//
// WHAT THE DEFECT WAS. This is the discard shape of #2639's family, left out of
// that fix's scope on purpose and filed. `lowerNew`'s imported branch
// (src/codegen/LlvmBackend_Exprs2.cpp, `if (ctx_.isImported(t))`) admits an
// argument list via an `allPositional` predicate; #2639 widened it to accept
// by-ref modifiers but kept `a.isDiscard` excluded. The branch's only other
// exit is a bare `return obj`, which emits NOTHING — so the instance was
// calloc'd, `bz$T$init` (vtable + field initializers) ran, and `bz$T$ctor` was
// never called. The read in the repro is of a field the CONSTRUCTOR ASSIGNS,
// not of the discarded recipient, so the symptom is the whole constructor body
// silently not running.
//
// Admitting `isDiscard` to that predicate alone would NOT have fixed it, which
// is why the carve-out was a real scoping decision: a discard has
// `value == nullptr` and would have hit the `if (!ne.args[ai].value)
// { ok = false; break; }` guard inside the loop and still emitted no call. The
// discard needs a SCRATCH SLOT, which §22.4 authorizes — "the receiving slot is
// internal to the compiler and is not observable" — and §22.3 lists `out _`
// among the accepted `out` argument forms.
//
// THE FIX has two parts, and the second is the one the review made a gating
// condition on closing this issue.
//
//   (1) MAKE IT WORK for a BORROWING by-ref formal (`ref` / `out`). The
//       predicate admits a discard whose imported formal is `ref` or `out`, and
//       the per-argument loop — BEFORE the `!value` guard — allocates a
//       throwaway scratch of the formal's type, zero-initializes it, and passes
//       its address. The type comes from the imported
//       `ParameterSymbol::resolvedDeclType`, the same field
//       `declareImportedCtor` builds the callee's own signature from, so the
//       scratch and the formal cannot disagree on width — the #2639/#2629
//       wild-write lesson applied up front rather than after a SIGSEGV.
//       This mirrors buildOrderedArgs' identical #1301 block
//       (LlvmBackend_Assign.cpp:7657) that the ordinary method path has had all
//       along; this hand-rolled loop simply never got it.
//
//       ⚠ ONLY a plain borrowing `out` formal is admitted. §17.13 sanctions
//       the discard in exactly three positions, and its out-argument position
//       is "As a positional argument with the `out` modifier" — `out`, not
//       `ref`. An earlier draft admitted `ref _` too and made it work; that
//       would have committed the backend to a form the spec does not define,
//       so it was narrowed. `ref _` then reached the loud residual (U3).
//       (SUPERSEDED at v0.244 by proposal 0083 / #3049: §17.13 now states
//       "`ref _` is a compile error" outright, so the form never reaches
//       codegen from any path and U3 pins the REJECTION instead — see U3.)
//
//   (2) MAKE THE RESIDUAL LOUD. An `out owned` formal's discard is still NOT
//       lowered, and that is a decline of a NORMATIVE obligation, recorded as
//       such rather than as a gap: §22.6 states "`out _`. The discard form
//       applies to both parameter kinds: against `out T` the received value is
//       simply dropped (§17.13); against `out owned T` the discarded value is
//       an unbound owned temporary disposed at the end of the full statement
//       (§28.3)." Implementing it needs the scratch-slot ownership bookkeeping
//       buildOrderedArgs performs (`discardOwnedOutSlots_` plus #1833(a)'s
//       unwind entry), and no oracle available here can see a cross-module leak
//       or double-free (ASan is blind past `-c`, see below), so shipping it
//       unverified is not an option. It is therefore loudly unimplemented
//       rather than silently so, and filed with the §22.6 target semantics.
//
//       Every exit from the imported branch that did not emit `bz$T$ctor` now
//       reports an `internal:` diagnostic. The predicate is "this imported TYPE
//       declares a constructor", NOT "ctorForArgs resolved one" — the
//       distinction is load-bearing and was a real defect in the first draft.
//       `ctorForArgs` (LlvmBackend_impl.hpp:7767-7843) reaches an imported
//       constructor only via its two ARITY-EXACT branches, because its
//       defaults/`argsCanFill` fallback at :7836-7841 is gated on
//       `dynamic_cast<const ast::ConstructorDecl*>(cs->decl)` — null for every
//       imported constructor, i.e. the same #2374 null-decl bug this family is
//       made of. Keyed on `cs`, the loudness guard contained an instance of the
//       very bug it exists to make loud. It still cannot fire for a genuinely
//       ctor-less imported type (U6 pins that direction; U5 pins that it stays
//       silent on an ordinary correct call).
//
//       The guard also covers the other shapes the review named as still
//       reaching the silent fall-through: NAMED arguments, by-value `owned` /
//       `scoped`, and any by-ref argument whose `lvalueAddr` returns null
//       without the #2319 bracket applying.
//
// ⚠ ASAN IS NOT THE ORACLE FOR THIS FAMILY. Recorded on #2629/#2639 and
// re-confirmed for #2657: the dependency artifact is built WITHOUT
// instrumentation (`--sanitize=address` is rejected together with `-c`), so a
// callee-side over-wide write or free is performed by uninstrumented code and
// ASan does not see it — a half-fixed #2639 build was ASan-CLEAN while
// SIGSEGVing on the plain path. Every row below was verified on the PLAIN
// build.
//
// MECHANICAL FAIL->PASS (plain path; this fix's hunks reverted with
// `git checkout --`, rebuild, re-run, then `git apply` of the byte-identical
// patch). NOTE the baseline is main WITH #2639 already applied — this commit
// depends on it:
//
//     case                                              pre-fix    post-fix
//     U1 `out _` vs `out n: int`, ctor-assigned field      0          3
//     U2 `out _` vs `out n: string` (8-byte formal)        0          8
//     U3 `ref _`: declined, diagnostic emitted          SILENT      loud
//        (re-pointed at v0.244/#3049: now a hard compile REJECT, see U3)
//     U4 `out owned` discard: diagnostic emitted        SILENT      loud
//     U5 control: no modifiers at all                     49         49
//     U6 control: imported type with NO constructor       60         60
//     U7 control: inline `out var` (the #2639 shape)      10         10
//     U8 control: pre-declared `out` recipient            10         10
//     U9 `out _` in a 5-iteration LOOP                     0         40
//
// FOUND WHILE CLOSING THE `ctorForArgs` HOLE, and NOT what was predicted. The
// expected counterexample — an imported constructor with a DEFAULT parameter
// called with fewer arguments — does not reach the silent fall-through at all:
// sema rejects it first with "`new WDefault` has 1 argument(s) but the
// parameter list has 2 (§22.5)". The identical class compiled IN-MODULE accepts
// `new W(1)` and returns 26, so §22.5's optional parameters are simply not
// honored across `--import`. That is loud (a hard error), so it is a different
// defect from this one and is FILED separately rather than pinned here — a test
// asserting the current rejection would lock in the bug.
//
// U1's expected value (3) is the constructor's assignment to `v`, deliberately
// NOT 0 and not equal to any other field default, so the "constructor body
// never ran" symptom cannot masquerade as a pass. U2 uses an 8-byte `string`
// formal for the same reason #2639's slot half existed: a scratch sized from
// the wrong source would be a 4-byte alloca the callee writes 8 bytes through.
//
// U6 is the guard's own false-positive test. `NoCtor` has no constructor at
// all, so `new NoCtor()` legitimately emits no `$ctor` call; if the loud guard
// were keyed on "no call emitted" alone rather than on "a constructor was
// selected but could not be lowered", this would produce a spurious diagnostic
// on correct code.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
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

struct TempFile2658 {
    std::string path;
    explicit TempFile2658(std::string p) : path(std::move(p)) {}
    ~TempFile2658() { std::remove(path.c_str()); }
};

void writeSource2658(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library. `W2`/`W7`/`W8` cover the borrowing by-ref formals at
// two widths; `WOwned` is the `out owned` formal whose discard stays unlowered
// (and must therefore be LOUD); `NoCtor` is the guard's false-positive control.
// Every constructor assigns `v` a distinct non-zero value so that "the ctor
// body never ran" can never be mistaken for a pass.
const char* kLib2658 =
    "module DiscCtorLib2658; "
    "public class Payload "
    "  public k: int; "
    "  public constructor(a: int) "
    "    k := a; "
    "  end; "
    "end Payload; "
    "public class W2 "
    "  public v: int; "
    "  public constructor(s: string, out n: int) "
    "    n := 7; v := 3; "
    "  end; "
    "end W2; "
    "public class W6 "
    "  public v: int; "
    "  public constructor(a: int) "
    "    v := a + 40; "
    "  end; "
    "end W6; "
    "public class W7 "
    "  public v: int; "
    "  public constructor(s: string, out n: string) "
    "    n := \"abcde\"; v := 8; "
    "  end; "
    "end W7; "
    "public class W8 "
    "  public v: int; "
    "  public constructor(ref n: int) "
    "    n := n + 10; v := 6; "
    "  end; "
    "end W8; "
    "public class WOwned "
    "  public v: int; "
    "  public constructor(s: string, out owned p: Payload) "
    "    p := new Payload(33); v := 5; "
    "  end; "
    "end WOwned; "
    "public class NoCtor "
    "  public v: int; "
    "end NoCtor;";

std::string appSource2658(const std::string& mainBody,
                          const std::string& moduleTag) {
    return "module DiscCtorApp2658" + moduleTag + "; import DiscCtorLib2658; "
           "class C "
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end C;";
}

// Build the dependency object once per case, compile the app against it, run
// the app in a SEPARATE process (fork-isolated, per tests/README.md).
// `capturedStderr`, when non-null, receives everything the compile wrote to
// stderr — that is where the driver prints diagnostics, and it is how U4 pins
// the loud residual. Returns the child's exit code; -1 on a compile/link/run
// failure; -2 when the dependency itself could not be produced (no toolchain).
int runImported2658(const std::string& mainBody, const std::string& moduleTag,
                    std::string* capturedStderr = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2658_" + moduleTag + "_" + unique_stem("");

    TempFile2658 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2658 aObj((dir / (stem + "_a.o")).string());
    writeSource2658(aSrc.path, kLib2658);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2658 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2658 bExe((dir / (stem + "_b")).string());
    TempFile2658 bErr((dir / (stem + "_b.err")).string());
    writeSource2658(bSrc.path, appSource2658(mainBody, moduleTag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        // Redirect fd 2 to a file for the duration of the compile, then restore
        // it. The driver prints diagnostics to stderr and CompileResult carries
        // only a bool + an error count, so this is the only way to assert on
        // the diagnostic TEXT — and a warning does not move either of those
        // fields, which is exactly what U4 needs to observe.
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) {
            ::dup2(saved, STDERR_FILENO);
            ::close(saved);
        }
        std::ifstream in(bErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *capturedStderr = ss.str();
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
    }
    if (!compiled) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// U1 — #2658's own repro. §22.3 lists `out _` among the accepted `out` argument
// forms; the read is of `v`, a field the CONSTRUCTOR ASSIGNS. Pre-fix the whole
// constructor body never ran, so `v` kept its zero default and the program
// returned 0 with NO diagnostic at all.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_out_discard_runs_constructor_body) {
    int rc = runImported2658(
        "    var w := new W2(\"x\", out _); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "U1");
    if (rc == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 3);
}

// ===========================================================================
// U2 — the same discard against an 8-BYTE formal (`out n: string`). The scratch
// slot is sized from the imported `ParameterSymbol::resolvedDeclType`; sizing
// it from anywhere else (or defaulting it) reproduces #2639's 4-byte-alloca
// wild write, which is why this width is pinned separately from U1's `int`.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_out_discard_string_formal_scratch_is_correctly_sized) {
    int rc = runImported2658(
        "    var w := new W7(\"x\", out _); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "U2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 8);
}

// ===========================================================================
// U3 — `ref _` is a COMPILE ERROR (§17.13, v0.244; #3049 / proposal 0083).
//
// History, because this case has now been re-pointed twice and the reason
// matters. #2658 narrowed its scratch-slot lowering to a plain borrowing `out`
// formal because §17.13's out-argument position reads "As a positional
// argument with the `out` modifier" — `out`, not `ref`. `ref _` therefore
// reached the loud `internal:` residual instead of being silently mis-lowered,
// and this case pinned that DECLINE. Whether sema should reject `ref _`
// outright was filed rather than settled (#2679).
//
// It is settled now, and in the direction that removes the residual entirely:
// §17.13 at v0.244 states it as a rule — "the discard composes with `out`
// only: **`ref _` is a compile error** — a `ref` argument reads its operand,
// which a discard cannot supply; pass a named location (§22.3)" — and A.4's
// ArgExpression note repeats it ("in every non-out position — `ref _` included
// — it is a compile error"). The form no longer reaches codegen from ANY path,
// imported or not, so there is no residual left to be loud about: the program
// is rejected in the parser.
//
// The case still pins a DIAGNOSTIC rather than a value (asserting only "wrong
// value" would pass identically pre-fix — the vacuity trap), just a different
// one: the compile must FAIL, naming `ref _` and its remedy.
// per spec §17.13 / §22.3 / A.4, v0.244
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_ref_discard_is_rejected_spec_17_13) {
    std::string err;
    int rc = runImported2658(
        "    var w := new W8(ref _); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "U3", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // the app did not compile
    BEZEL_EXPECT_TRUE(err.find("`ref _` is a compile error") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("pass a named location") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("\xc2\xa7""17.13") != std::string::npos);
}

// ===========================================================================
// U4 — THE GATING CONDITION. An `out owned` formal's discard is deliberately
// still not lowered (it needs ownership bookkeeping this path does not have and
// that cannot be verified across a module boundary). What must NOT happen is
// that it stays SILENT: pre-fix this program compiled clean, emitted no
// diagnostic of any kind, and returned a wrong answer. The residual is now
// reported.
//
// Asserting the diagnostic TEXT rather than a coarse pass/fail is required
// here twice over: a Warning moves neither `CompileResult::success` nor
// `errorCount`, and the value returned is wrong either way, so the message is
// the only observable that distinguishes fixed from broken.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_out_owned_discard_residual_is_not_silent) {
    std::string err;
    int rc = runImported2658(
        "    var w := new WOwned(\"x\", out _); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "U4", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_TRUE(err.find("#2658") != std::string::npos);
    BEZEL_EXPECT_TRUE(
        err.find("no constructor call could be lowered") != std::string::npos);
}

// ===========================================================================
// U5 — CONTROL: an imported constructor with no argument modifiers at all. The
// broadest guard that neither the widened predicate nor the new diagnostic
// disturbs ordinary cross-module construction.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_without_modifiers_still_correct) {
    std::string err;
    int rc = runImported2658(
        "    var w := new W6(9); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; ", "U5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 49);
    // The loud guard is keyed on "this imported TYPE has a constructor", which
    // is broader than "ctorForArgs resolved one" (see U9's note). Assert it
    // stays silent on the ordinary, correctly-lowered call — the shape it would
    // most damage if the widening were wrong.
    BEZEL_EXPECT_TRUE(err.find("#2658") == std::string::npos);
}

// ===========================================================================
// U6 — CONTROL, and the loud guard's own FALSE-POSITIVE test. `NoCtor` declares
// no constructor, so `new NoCtor()` legitimately emits no `$ctor` call. The
// guard is keyed on "a constructor was SELECTED for these arguments but could
// not be lowered", not on "no call was emitted", so it must stay silent here.
// A guard written the other way would fire on every correct construction of a
// ctor-less imported type.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_type_without_constructor_gets_no_diagnostic) {
    std::string err;
    int rc = runImported2658(
        "    var w := new NoCtor(); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q + 60; ", "U6", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 60);
    BEZEL_EXPECT_TRUE(err.find("#2658") == std::string::npos);
}

// ===========================================================================
// U7 — CONTROL: the inline `out var` shape #2639 fixed, on the same
// constructor U1 uses. Pins that reworking the `allPositional` loop into a
// per-argument form did not disturb the by-ref admission #2639 added.
// (`w.v` is 3 and `n` is 7, so a skipped ctor body cannot reach 10.)
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_inline_out_var_still_correct) {
    int rc = runImported2658(
        "    var w := new W2(\"x\", out var n); "
        "    var q := w.v + n; "
        "    _ := w; "
        "    return q; ", "U7");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 10);
}

// ===========================================================================
// U8 — CONTROL: a PRE-DECLARED `out n` recipient, the other by-ref shape #2639
// repaired. Its initializer is 0 and the constructor writes 7, so the sum with
// `w.v` (3) distinguishes a skipped call (0) from a completed one (10).
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_predeclared_out_still_correct) {
    int rc = runImported2658(
        "    var n: int := 0; "
        "    var w := new W2(\"x\", out n); "
        "    var q := w.v + n; "
        "    _ := w; "
        "    return q; ", "U8");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 10);
}

// ===========================================================================
// U9 — THE LOOP SHAPE, and the reason the scratch's zero-store lives in the
// ENTRY block rather than at the call site.
//
// `entryAlloca` (LlvmBackend_impl.hpp:2029-2033) hoists the scratch alloca to
// the function's entry block, so all five iterations share ONE slot. A store
// emitted at the call site would sit INSIDE the loop body and re-null that slot
// every iteration — wiping the pointer the previous iteration left there before
// the imported callee's release-of-prior on `n := …`
// (LlvmBackend_Calls2.cpp:1660-1668) could consume it, leaking one string per
// iteration. `ensureOutVarSlots` already measured exactly this and split on it
// (its comment at LlvmBackend_Calls2.cpp:1685-1697): "call-site store leaks 5
// objects (195 B), entry-block store leaks 1 (39 B)". Pre-fix no call was
// emitted at all, so a call-site store here would have been a NEWLY reachable
// leak rather than a pre-existing one.
//
// Verified by IR rather than by a sanitizer, which matters because ASan is
// blind across `--import` (see the header note). `BEZEL_DUMP_IR=1` on this
// program, call-site store versus entry-block store:
//
//   call-site (rejected)            entry-block (shipped)
//   entry:                          entry:
//     %1 = alloca ptr, align 8        %1 = alloca ptr, align 8
//     ...                             store ptr null, ptr %1, align 8
//     br label %while.cond            ...
//   while.body:                       br label %while.cond
//     store ptr null, ptr %1  <—    while.body:
//     call ... W7$ctor(..., ptr %1)   call ... W7$ctor(..., ptr %1)
//
// The value assertion below is what this test can observe on the plain path: 5
// iterations x `v == 8` == 40 proves the constructor body ran on EVERY
// iteration, which a shared-slot bug of the opposite kind (slot clobbered so
// the call is skipped or faults) would break. The leak itself is not observable
// from an exit code — it is pinned by the IR contrast above and by the measured
// precedent this placement copies.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_discard_2658,
           imported_ctor_out_discard_in_a_loop_runs_every_iteration) {
    int rc = runImported2658(
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 5 do "
        "      var w := new W7(\"x\", out _); "
        "      total := total + w.v; "
        "      _ := w; "
        "      i := i + 1; "
        "    end while; "
        "    return total; ", "U9");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 40);
}

}  // namespace bezel::module
