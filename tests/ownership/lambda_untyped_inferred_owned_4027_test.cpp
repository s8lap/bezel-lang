// Spec-derived regression tests for issue #4027 — an untyped lambda literal
// (`var f := lambda() => new Res(9);`, with NO `() -> owned T` target
// anywhere in the chain) infers its own function type with NO `owned`
// marker on `funcReturnOwned`, even when its body unconditionally returns
// a fresh owned production. Calling `f()` then classifies the result
// BORROWED, so the freshly-allocated `Res` is never tracked for disposal
// and LEAKS silently — no diagnostic, no leak message at compile time, just
// a 16-byte leak at runtime.
//
// ROOT CAUSE (Ownership.cpp, the `producerState` arm at ~line 4589):
//   `exprType(lam)->funcReturnOwned` is the only signal consulted for an
//   untyped lambda whose callee TypeRef is Function-shaped (the
//   `calleeTy->kind == sem::Function && calleeTy->funcReturnOwned` check).
//   Sema's LambdaExpr arm (ExprTypecheck_Exprs.cpp ~line 9610) sets
//   `lamTy->funcReturn = inferredReturn;` with no accompanying
//   `funcReturnOwned` assignment — the inferred TypeRef never carries the
//   marker even when the body's inferred return IS a fresh owned production.
//   The producerState arm therefore classifies the call result BORROWED,
//   the local at the call site (`var c := f();`) is not flagged Owned, and
//   no scope-end Dispose is registered. The fresh `Res` dies at program
//   exit without ever being freed.
//
// THE FIX (sema-side, ExprTypecheck_Exprs.cpp LambdaExpr arm): when the
// lambda literal has NO target function type whose return slot is marked
// `owned` (lamTargetRetOwned is false), and the body's every return path
// IS a fresh owned production (a `new` constructor call, an `owned`-returning
// call, etc.), set `lamTy->funcReturnOwned = true` on the inferred
// function type so the call-result classifier at producerState picks it
// up. This mirrors the named-method analogue in `staticFactoryReturnsFresh
// Owned` (Ownership.cpp ~line 4405, consulted by producerState for any
// non-`owned` static return whose body is unambiguously fresh) — the
// lambda form was simply never wired into the same signal.
//
// §28.3 *Factory methods and sourceless returns*, v0.294: a static
// `owned`-returning declaration's caller-side classification is read off
// the declared return type with no heuristic. An untyped lambda literal
// with no target is the lambda-side analogue — its body's producer
// classification IS the only signal available, and the spec's
// declaration-site obligation for factories has its lambda-side
// counterpart in this fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Same destructor-counter Res as #4005 — ctor ++, dtor --, surfaced through
// `Res.live` so an ACCEPT test can assert exact-once disposal (0 == no leak,
// no double-free). A SINGLE iteration suffices to demonstrate the leak on
// the fail-without: the loop-amplify to 500x on the accept-with, identical
// pattern to #4005's own `own_local_move_alias_*_disposes_once` test.
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH — the issue's exact repro. An untyped lambda
// whose body unconditionally returns a fresh `new` production must be
// inferred as `() -> owned Res` so its call result is tracked Owned and
// disposed at scope end. Loop-amplify 500x so a leak is unambiguous (one
// leaked object per iteration drives the final `Res.live` to 500, not 0).
// ===========================================================================
BEZEL_TEST(lambda_untyped_inferred_owned_4027,
           untyped_lambda_fresh_new_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda() => new Res(9); "
        "      do using var c := f(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH — the body is an `owned`-returning named call
// (the OTHER fresh-production shape, not a direct `new`). An untyped lambda
// returning such a call must also be inferred as `() -> owned Res`. Same
// loop-amplify to 500x.
// ===========================================================================
BEZEL_TEST(lambda_untyped_inferred_owned_4027,
           untyped_lambda_owned_returning_call_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Make(): owned Res return new Res(11); end Make; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda() => Make(); "
        "      do using var c := f(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — a TYPED-LOCAL lambda whose target ALREADY carries
// `owned` (`var mk: () -> owned Res := lambda() => new Res(9);`) must
// continue to behave exactly as before. The fix is gated on
// `!lamTargetRetOwned`, so the target-side wiring (#4005 et seq.) is
// untouched. Loop-amplify 500x — exact-once disposal expected.
// ===========================================================================
BEZEL_TEST(lambda_untyped_inferred_owned_4027,
           typed_local_owned_target_still_works_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var mk: () -> owned Res := lambda() => new Res(9); "
        "      do using var c := mk(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// ===========================================================================
// NON-REGRESSION — a lambda with a NON-`owned` concrete-class target slot
// (`var mk: () -> Res := lambda() => new Res(9);`) must continue to be
// REJECTED by the existing concrete-slot rule (§28.3 *Concrete-slot function
// bodies*: "A fresh owned value … is rejected at the return path"), because
// the fix is gated on `!lamTargetRetOwned` — a target IS present here, just
// not marked `owned`, and the body IS a fresh production the slot does not
// announce as transferring.
// ===========================================================================
BEZEL_TEST(lambda_untyped_inferred_owned_4027,
           typed_local_non_owned_target_still_rejects_spec_28_3) {
    JitResult jr = runAot(
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var mk: () -> Res := lambda() => new Res(9); "
        "    var c := mk(); return c.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
}

// ===========================================================================
// NON-REGRESSION — a lambda body whose return is NOT a fresh production
// (a literal int return, with NO capture so the closure-cell leak, a
// separate pre-existing issue, does not interfere) must NOT be marked
// `funcReturnOwned` by the fix — the body is not an owned producer at all,
// so the inferred function type stays a plain (non-`owned`) `() -> int`.
// Calling `f()` then classifies BORROWED, which is correct for an int
// return (primitives carry no obligation). Loop-amplify 500x to demonstrate
// the absence of spurious rejections or diagnostics.
// ===========================================================================
BEZEL_TEST(lambda_untyped_inferred_owned_4027,
           untyped_lambda_int_body_not_owned_spec_28_3) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda() => 7; "
        "      var x := f(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}