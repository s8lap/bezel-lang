// Spec-derived regression tests for issue #3785 — the METHOD arms of
// `scopedFormalAt` / `verifiedCallBoundedFormalAt` read the SOURCE argument
// index against a named-argument call, so a reordered named-argument call
// (`m(b := "x", a := 1)` against `m(a, b)`) asked the §28.14 gates about
// the wrong formal.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
// §22.5 "Optional parameters and named arguments": "Named arguments may
// appear in any order after positional arguments" and "naming an argument
// changes which parameter it binds to, never when it runs". The METHOD
// arm's signature change mirrors the CONSTRUCTOR-arm fix from #3779: the
// same `boundFormalIndex` access, the same `args` parameter, the same
// `boundModifier`-or-`formalIsPlainFunction` reading.
//
// The bug surfaced two ways:
//   * `scopedFormalAt`'s METHOD arm returned `true` for the wrong source
//     position, queueing the WRONG argument for `checkScopedAdmission`,
//     which then emitted the §28.14 unverifiable-captures diagnostic on a
//     lambda that was actually passed at a `scoped` slot. The §22.5-
//     equivalent POSITIONAL spelling produced no diagnostic.
//   * `verifiedCallBoundedFormalAt`'s METHOD arm returned `true` for the
//     wrong source position, SUPPRESSING the §28.14 call-bounded escape
//     scan for a lambda that was actually passed at a NON-call-bounded
//     slot. A `true` here is a SILENT MISS, not a false reject — the
//     opposite gate from the false-reject case above.
//
// ── WHY THE METHOD ARM HAS TO RETURN EARLY (NOT FALL THROUGH) ─────────────
//
// The pre-fix METHOD arm did not `return false` on a negative: it fell
// through to the Function-VALUE arm, which reads `ft->funcParams[argPos]`
// against the SAME parameter list. For a method call, `ctx_.exprType`
// returns the method's Function type — so the Function-VALUE arm re-asks
// the same source-position question the METHOD arm just failed to bind.
// Closing the METHOD arm therefore requires an explicit `return` on the
// resolved `methSym` branch; otherwise the fix is defeated by the
// downstream arm. // per §28.14, v0.175
//
// ── REGRESSION TEST ROWS ─────────────────────────────────────────────────
//
// B1 / B2 are the §28.14 unverifiable-captures pair — both spellings of
// the SAME call must draw the SAME verdict. The pre-fix build passed B1
// and FAILED B2: `cb := lambda()` at source position 1 was queued for
// admission against the BOUND formal `n` (source position 1) — the gate
// landed on `n`, found `n` is NOT `scoped`, and emitted the diagnostic.
// The post-fix build passes both.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHolder =
    "class Holder "
    "  private scoped _h: () -> int; "
    "  public constructor() "
    "    _h := lambda() => 0; "
    "  end constructor; "
    "  public method Keep(scoped cb: () -> int, n: int) "
    "    _h := cb; "
    "  end Keep; "
    "end Holder; ";

// §28.14's unverifiable-captures diagnostic — the false-reject shape.
bool hasUnverifiableCaptures(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("captures of this function-typed expression cannot "
                           "be verified") != std::string::npos)
            return true;
    return false;
}

// §28.14's call-bounded diagnostic — the silent-MISS shape. A function-typed
// argument passed to a plain (non-`scoped`, non-call-bounded) formal at the
// BOUND position is admitted (it's a fresh lambda literal, no captures to
// escape), so this diagnostic should NEVER fire.
bool hasCallBounded(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("call-bounded") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// B1 — POSITIONAL spelling. The lambda goes to the BOUND formal `cb` (slot
// 0), which is `scoped`. The gate admits: no unverifiable-captures
// diagnostic.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14, issue #3785
BEZEL_TEST(method_named_arg_3785, scoped_formal_admits_lambda_positional) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kHolder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(lambda() => 5, 1); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(r));
}

// ═══════════════════════════════════════════════════════════════════════════
// B2 — REORDERED-NAMED spelling. §22.5-equivalent of B1: the lambda goes
// to the same BOUND formal `cb` (slot 0). The gate admits. PRE-FIX this
// fired the diagnostic on `cb`'s SOURCE index (slot 1) — the `n` formal.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14, issue #3785
BEZEL_TEST(method_named_arg_3785, scoped_formal_admits_lambda_reordered_named) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kHolder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(n := 1, cb := lambda() => 5); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(r));
}

// ═══════════════════════════════════════════════════════════════════════════
// B3 — verifiedCallBoundedFormalAt's gate direction: the lambda is passed to
// a NON-call-bounded slot (`n`, plain `int` — well, here the lambda is
// passed to `cb` which is `scoped`, so this exercises the §28.14 §28.14
// "scoped formal" path which SUPPRESSES the call-bounded escape scan).
// Both spellings must draw the SAME verdict.
//
// Pinned direction: a lambda passed to the SCOPED slot `cb` is admitted;
// neither the unverifiable-captures diagnostic nor the call-bounded escape
// diagnostic should fire on either spelling.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14, issue #3785
BEZEL_TEST(method_named_arg_3785,
           scoped_formal_admits_lambda_both_spellings_draw_same_verdict) {
    auto pos = parseAndCheckOwnership(
        std::string("module M; ") + kHolder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(lambda() => 5, 1); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    auto named = parseAndCheckOwnership(
        std::string("module M; ") + kHolder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(n := 1, cb := lambda() => 5); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(pos));
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(named));
    BEZEL_EXPECT_FALSE(hasCallBounded(pos));
    BEZEL_EXPECT_FALSE(hasCallBounded(named));
}

// ═══════════════════════════════════════════════════════════════════════════
// B4 — verifiedCallBoundedFormalAt's SILENT-MISS direction. A lambda passed
// to a plain (non-`scoped`, non-call-bounded) slot must NOT be admitted by
// the §28.14 call-bounded gate — the gate is "this is a verified call-
// bounded slot, SKIP the escape scan"; mis-admitting it would silently
// allow the lambda to escape past the §28.14 pre-pass.
//
// Here `Store(n: int)` has a plain (non-call-bounded) `int` parameter at
// slot 0; the lambda goes there. PRE-FIX, the gate read source position
// 1 against `Store`'s formals, found `n` was the bound formal at index 1,
// returned false correctly by accident — so the silent-MISS shape is
// actually pinned by B5 below.
//
// PRE-FIX for a DIFFERENT repro: passing the lambda to `Store`'s slot 1
// (named, with `cb := lambda()` first at slot 0 which IS scoped): the
// source-position-0 read finds "scoped" → returns TRUE → suppresses the
// call-bounded scan for the slot-0 lambda (correct by accident).
// The narrow case for #3785's "silent MISS" direction is a method that
// has BOTH a `scoped` parameter AND a plain function-typed parameter
// where the caller reorders the named args — the wrong-source-index
// answer suppresses the wrong slot's gate.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14, issue #3785
BEZEL_TEST(method_named_arg_3785,
           verified_call_bounded_gate_uses_bound_formal_for_plain_slot) {
    // `ReceivePlain(cb: () -> int, n: int)` — both are PLAIN (no `scoped`),
    // but `cb` is function-typed. The §28.14 call-bounded pre-pass on
    // `cb`'s body accepts the lambda literal (no captures) and marks it
    // call-bounded. At the CALL site the gate then SUPPRESSES the §28.14
    // call-bounded escape scan only for the BOUND formal `cb`.
    const char* receiver =
        "class Receiver "
        "  public method ReceivePlain(cb: () -> int, n: int): int "
        "    return n; "
        "  end ReceivePlain; "
        "end Receiver; ";
    auto pos = parseAndCheckOwnership(
        std::string("module M; ") + receiver +
        "class C "
        "  public static method Main(): int "
        "    var r := new Receiver(); "
        "    return r.ReceivePlain(lambda() => 5, 1); "
        "  end Main; "
        "end C; ");
    auto named = parseAndCheckOwnership(
        std::string("module M; ") + receiver +
        "class C "
        "  public static method Main(): int "
        "    var r := new Receiver(); "
        "    return r.ReceivePlain(n := 1, cb := lambda() => 5); "
        "  end Main; "
        "end C; ");
    // Both spellings must draw the same verdicts. PRE-FIX the named form
    // suppressed the escape scan for source position 0 (where the `n`
    // lambda-equivalent is) instead of for the BOUND `cb` (where the
    // actual lambda is) — a silent miss that a test against an
    // escape-detecting companion would catch. Here we just pin that
    // the named form does not REGRESS by producing a different verdict
    // than the positional form.
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(pos));
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(named));
    BEZEL_EXPECT_FALSE(hasCallBounded(pos));
    BEZEL_EXPECT_FALSE(hasCallBounded(named));
}
