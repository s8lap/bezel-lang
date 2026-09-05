// fix #3726 — stdlib §5 LINQ operator at a BOGUS ARITY silently
// zero-degrades (stdlib §5 `System.Collections.Linq`, v0.286).
//
// WHAT PRE-FIX ACCEPTED. The §5 name exemption in
// `kBuiltinCollectionLinqOperatorNames` (BuiltinRegistry.cpp) is name-only,
// so a `Where(1, 2)` / `Take(1, 2)` / `Where()` call slipped past
// `typeLinqOperator`'s name-only pass-through match, was typed as
// `IEnumerable<T>` UNCONDITIONALLY (no arity check anywhere in the
// lambda), and codegen's #2434/#2491 zero-init degradation notes let it
// compile rc=0, link, and run with `count=0` — a silent zero-degrade on
// a LIVE stdlib §5 operator. The §5 module has the canonical arity for
// every operator, so the fix is a one-table lookup the existing §26.5
// extension-ambiguity path was ALREADY doing, just hoisted into a header
// (`sema/LinqStdlibOverloads.hpp`) so this lambda could consult the
// SAME source of truth.
//
// WHAT THE FIX REJECTS, AND WHAT IT DOES NOT. The arity gate runs ONLY
// when (a) `m` is in the stdlib §5 name exemption (so non-LINQ names like
// `Add` / `Remove` / `Nonexistent` continue to fall through to the
// existing "no member" path), (b) `m` is not `OfType` (withdrawn at
// v0.284 by fix #3559; the dedicated arm reports the v0.284 removal
// wording at ANY arity, fix #3710), and (c) the hashable gate above it
// did not already fire (Distinct/Union/Intersect/Except/Contains/ToSet
// on a non-hashable T must keep the hashable diagnostic, not be
// silently redirected to "wrong number of arguments"). Every row below
// pins one of these boundaries.
//
// ROW COUNT. The brief asked for 6–7 rows; this file has 7:
//   Row 1 — `Where(1, 2)` rejected, located, no executable (#2434 note
//           must NOT fire).
//   Row 2 — `Take(1, 2)` rejected.
//   Row 3 — `Where()` (zero-arg) rejected.
//   Row 4 — `Where(predicate)` (correct arity) accepted by VALUE — the
//           #2434 diagnostic must not appear, AND the program must run.
//   Row 5 — `Take(count)` (correct arity) accepted by VALUE.
//   Row 6 — `Nonexistent<int>(1)` REJECTED with the existing "no member"
//           wording — proves the gate did not regress non-LINQ names.
//   Row 7 — Row 4 amplified to 500 iterations — proves the fix did not
//           introduce a hashable-gate-style flapping regression on the
//           control path (see [[lsan-one-shot-leak-tests-pass-vacuously]]
//           for why one-shot positive tests are not enough).
//
// WHY THIS IS A SEMANTIC TEST (parseSemaAll) AND NOT JUST A BEZEL AOT
// RUN-AOT TEST. The diagnostic is a SEMANTIC reject (`Severity::Error`,
// the §22.5 "wrong number of arguments" wording), and the diagnostic's
// TEXT — and its absence on the controls — is what pins the fix.
// runAot synthesises an `Error` diagnostic for a runtime crash too
// (the pre-fix behaviour WAS a codegen crash, so an `r.diagnostics.
// hasErrors()` check would be vacuous — exactly the failure mode
// `tests/sema/linq_contains_nullable_arg_2593_test.cpp` warns about,
// lines 23–28), so the negative rows must assert on the diagnostic
// STRING. The positive rows use runAot because the value-side proof
// `count=2` distinguishes "compile + run as expected" from
// "compile + run with zero-degrade", which a `hasErrors() == false`
// check cannot.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

constexpr const char* kWrongArityDiag =
    "wrong number of arguments for stdlib §5 LINQ operator";

constexpr const char* kInternalLowerDiag =
    "internal: lowerLocalDecl's initializer expression failed to lower";

// The accepted-arity host body. Row 4 / Row 5 build on this; Row 7
// repeats Row 4. Kept as a function so the §22.5 `"using var"` lvalue
// initialiser the §5 operator types is exercised — Row 1–3's bug was
// the LVALUE initialiser silently zero-degrading (the operator's
// typed result never reached the slot).
std::string acceptedBody() {
    return "    using var a := new Array<int>(); "
           "    a.Add(1); a.Add(2); a.Add(3); "
           "    using var b := a.Where(lambda(x: int): bool => x > 1).ToArray(); "
           "    Console.WriteLine($\"count={b.Count}\"); "
           "    return 0;";
}

std::string acceptedBodyTake() {
    return "    using var a := new Array<int>(); "
           "    a.Add(1); a.Add(2); a.Add(3); "
           "    using var b := a.Take(2).ToArray(); "
           "    Console.WriteLine($\"count={b.Count}\"); "
           "    return 0;";
}

constexpr const char* kPrelude =
    "module M; "
    "class C "
    "  public static method Main(): int ";

constexpr const char* kSuffix = "  end Main; end C;";

std::string prog(const std::string& body) {
    return std::string(kPrelude) + body + kSuffix;
}

}  // namespace

// ============================================================================
// Row 1 — `a.Where(1, 2).ToArray()` rejected.
// Pre-fix: rc=0, "internal: lowerLocalDecl's initializer expression failed to
// lower to a value for this local's slot (degraded to a zero-initialized
// value) — #2434" + executable + "count=0".
// Post-fix: located "wrong number of arguments for stdlib §5 LINQ operator
// `Where` ... got 2, expected 1 (#3726)" + NO executable + the
// lowerLocalDecl internal note MUST NOT fire (the new gate types the
// rejection before codegen's slot-init fold can swallow it).
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           where_two_args_rejected_with_arity_diagnostic) {
    auto r = parseSemaAll(prog(
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    using var b := a.Where(1, 2).ToArray(); "
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kWrongArityDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "got 2, expected 1"));
    BEZEL_EXPECT_TRUE(hasError(r, "`Where`"));
    BEZEL_EXPECT_TRUE(hasError(r, "stdlib §5 `System.Collections.Linq`"));
    BEZEL_EXPECT_TRUE(hasError(r, "v0.286"));
    BEZEL_EXPECT_TRUE(hasError(r, "(#3726)"));
    // The codegen-zero-init degradation note is the bug; it must not appear.
    BEZEL_EXPECT_FALSE(hasError(r, kInternalLowerDiag));
}

// ============================================================================
// Row 2 — `a.Take(1, 2).ToArray()` rejected (different stdlib §5 operator,
// same shape — pins the gate is `(name, arity)`-aware, not just `Where`).
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           take_two_args_rejected_with_arity_diagnostic) {
    auto r = parseSemaAll(prog(
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    using var b := a.Take(1, 2).ToArray(); "
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kWrongArityDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "got 2, expected 1"));
    BEZEL_EXPECT_TRUE(hasError(r, "`Take`"));
    BEZEL_EXPECT_FALSE(hasError(r, kInternalLowerDiag));
}

// ============================================================================
// Row 3 — `a.Where()` (zero-arg) rejected. Pre-fix the §5 name-only
// exemption admitted this too; post-fix, "got 0, expected 1".
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           where_zero_args_rejected_with_arity_diagnostic) {
    auto r = parseSemaAll(prog(
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    using var b := a.Where().ToArray(); "
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kWrongArityDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "got 0, expected 1"));
    BEZEL_EXPECT_TRUE(hasError(r, "`Where`"));
    BEZEL_EXPECT_FALSE(hasError(r, kInternalLowerDiag));
}

// ============================================================================
// Row 4 — CONTROL: `a.Where(lambda).ToArray()` accepted BY VALUE.
// The fix MUST NOT reject a correct-arity call (the §5 arity table is the
// same table the §26.5 ambiguity path consults, and ambiguity resolution
// was already correct). "Accepted" is proved by `count=2` — a hasErrors()
// check would pass vacuously if codegen zero-degraded (pre-fix shape).
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           where_predicate_accepted_runs_with_count_two) {
    auto r = bezel::test::runAot(prog(acceptedBody()));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    // Output check: must be "count=2", not "count=0" (the pre-fix
    // zero-degrade trap) and not "count=3" (a true silent bug).
    std::string out = r.ran ? r.stdoutText : std::string();
    BEZEL_EXPECT_TRUE(out.find("count=2") != std::string::npos);
}

// ============================================================================
// Row 5 — CONTROL: `a.Take(2).ToArray()` accepted BY VALUE. Different
// §5 operator from Row 4 (and a different admitted arity shape: `Take`
// admits only 1 arg in the table — 0-arg `Take()` is rejected by Row 3's
// shape for `Where` only because `Where`'s arity is uniquely 1).
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           take_count_accepted_runs_with_count_two) {
    auto r = bezel::test::runAot(prog(acceptedBodyTake()));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    std::string out = r.ran ? r.stdoutText : std::string();
    BEZEL_EXPECT_TRUE(out.find("count=2") != std::string::npos);
}

// ============================================================================
// Row 6 — CONTROL: a name NOT in the §5 name exemption (`Nonexistent`)
// must still hit the existing "no member" reject. This pins the gate's
// `isBuiltinCollectionLinqOperatorName(m)` scope — without it, every
// method on every collection would be arity-checked and the existing
// diagnostic for unrelated unknown members would be replaced with
// "wrong number of arguments".
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           nonexistent_member_still_no_member_reject) {
    auto r = parseSemaAll(prog(
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    var x := a.Nonexistent<int>(1); "
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Nonexistent`"));
    // The arity-gate diagnostic MUST NOT fire on a non-LINQ name — if
    // it did, the `isBuiltinCollectionLinqOperatorName` scope check is
    // missing and the gate is mis-firing on every method.
    BEZEL_EXPECT_FALSE(hasError(r, kWrongArityDiag));
}

// ============================================================================
// Row 7 — AMPLIFICATION: Row 4 repeated 500 times. Catches a
// hashable-gate-style flapping regression the one-shot Row 4 misses
// (cf. [[lsan-one-shot-leak-tests-pass-vacuously]]: a one-shot
// positive test can pass vacuously under a flapping gate). Each
// iteration is a fresh compile + run, so a 500× clean pass is the
// load-bearing signal that the gate did not introduce a non-determinism
// in the new `(name, arity)` lookup on the correct-arity path.
// ============================================================================
BEZEL_TEST(sema_linq_operator_arity_3726,
           where_predicate_accepted_runs_with_count_two_x500) {
    constexpr int kIters = 500;
    for (int i = 0; i < kIters; ++i) {
        auto r = bezel::test::runAot(prog(acceptedBody()));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
        std::string out = r.ran ? r.stdoutText : std::string();
        BEZEL_EXPECT_TRUE(out.find("count=2") != std::string::npos);
    }
}
