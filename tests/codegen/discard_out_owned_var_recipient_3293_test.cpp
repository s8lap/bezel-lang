// Codegen — issue #3293 / spec §17.13 (v0.266) + §22.3 / §28.3:
// the DISCARD form `_ := new Widget(out owned var x);` must keep
// `x` (the ctor's `out owned var` recipient) live for the
// enclosing BLOCK scope — exactly as the bare-statement form
// `new Widget(out owned var x);` does since #2681. The pre-fix
// `DiscardStmt::disposeOwnedValue` arm at
// `src/codegen/LlvmBackend_Stmts.cpp:2873` opened its one-entry
// dispose scope BEFORE `lowerExpr(*ds->value)`. That open-scope-
// first ordering re-homed `ensureCtorOutVarSlots`'s BLOCK-scope
// registration (the `out owned var x` slot disposal) into the
// STATEMENT scope, so the slot was disposed-and-nulled at end of
// the discard statement — and a subsequent `x.Id` read on the
// still-in-scope `x` dereferenced null → SEGFAULT.
//
// Pre-fix behavior (measured on `07e0cf7` with the unchanged
// `DiscardStmt::disposeOwnedValue` arm):
//   _ := new Widget(out owned var x);
//   var v := x.Id;
//   return v;          // SEGV (exit 139) — slot nulled at statement end
//
// The fix reorders to match the `ExpressionStmt::disposeOwnedValue`
// shape at `LlvmBackend_Stmts.cpp:2717/2721`:
// `lowerExpr(*ds->value)` FIRST, then emplace the one-entry dispose
// scope. With that, the ctor's `ensureCtorOutVarSlots` BLOCK-scope
// registration stays in the BLOCK scope, and `x` is live until the
// block's end — mirroring the bare-statement form's behavior, which
// is observed to return 9.
//
// The test pins BOTH the discard-form observable (no SEGV, returns 9)
// AND the bare-statement form's continued correctness (returns 9) so
// the reorder cannot regress either side. The bare-statement form's
// working value is from #2681's own test
// (`bare_new_trivial_stmt_reclaim_2655_test.cpp`).
//
// per §17.13, §22.3, §28.3 (issue #3293, spec v0.266)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// --- DISCARD form -----------------------------------------------------------

// §17.13 / `_ := new Widget(out owned var x);` followed by `x.Id`.
// Pre-fix: SEGV (slot was nulled at end of discard statement by the
// open-scope-first re-home into the STATEMENT scope). Post-fix: 9,
// because the ctor's `out owned var` recipient stays live until the
// block's end (mirrors the bare-statement form's observable since
// #2681).
BEZEL_TEST(discard_out_owned_var_recipient_3293,
           discard_keeps_recipient_live_until_block_end) {
    auto r = runAot(R"(
module M;
class Res
  public static alive: int := 0;
  public Id: int;
  public constructor(id: int) Id := id;
    Res.alive := Res.alive + 1; end constructor;
  destructor Res.alive := Res.alive - 1; end destructor;
end Res;
class Widget
  public constructor(out owned n: Res)
    n := new Res(9);
  end constructor;
end Widget;
class C
  public static method Main(): int
    _ := new Widget(out var x);
    var v := x.Id;
    return v;
  end Main;
end C;
)");
    // `r.ran == true` proves the binary executed (no SEGV — a SEGV
    // surfaces as `r.ran == false` in the harness). `exitCode == 9`
    // pins the value `x.Id` read AFTER the discard — it would be a
    // null deref read pre-fix, and any other value post-fix would
    // mean the ctor's recipient assignment was lost.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// --- BARE statement form (control — must continue to work) -----------------

// §17.13 / bare-statement `new Widget(out owned var x);` followed
// by `x.Id`. The bare form has been correctly returning 9 since
// #2681 (this is the case the existing #2655 test pins). This case
// pins that the reorder in `DiscardStmt::disposeOwnedValue` did
// not change the bare form's path — it lives in `lowerStmt`'s
// `ExpressionStmt` arm, NOT the `DiscardStmt` arm, and the reorder
// is scoped narrowly to that one arm.
BEZEL_TEST(discard_out_owned_var_recipient_3293,
           bare_stmt_form_still_correct) {
    auto r = runAot(R"(
module M;
class Res
  public static alive: int := 0;
  public Id: int;
  public constructor(id: int) Id := id;
    Res.alive := Res.alive + 1; end constructor;
  destructor Res.alive := Res.alive - 1; end destructor;
end Res;
class Widget
  public constructor(out owned n: Res)
    n := new Res(9);
  end constructor;
end Widget;
class C
  public static method Main(): int
    new Widget(out var x);
    var v := x.Id;
    return v;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

}  // namespace
