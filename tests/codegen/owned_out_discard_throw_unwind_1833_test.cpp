// fix #1833 (a) / §22.4 / §28.3 / §17.13 (spec v0.226): a callee that
// ASSIGNS a fresh owned heap value to an `out owned` parameter and then
// THROWS leaks the value on the unwind edge when the call site uses
// `out _` (discard) — the #1467 fix's normal-path `discardOwnedOutSlots_`
// loop only fires when the callee RETURNS. Per §28.3 ("the exception edge
// performs the same implicit disposal as the normal edge, on the single
// establishment-ordered cleanup stack"), once the callee writes to the
// out slot the value's ownership has transferred to the caller; the
// caller's ordinary exception-path discharge must reclaim it on the
// unwind edge.
//
// Verified on stock main (worktree HEAD 19ec167) with
// `bezelc --sanitize=address` + LSan: the throw path leaks the just-
// written Res (LSan Direct leak in `bz$W$TryMakeThrow`); the fix's
// `unwindOnly dynamicDispose` entry on the same scratch slot closes the
// leak on the unwind path. The normal-path behavior is unchanged
// (`unwindOnly=true` skips normal; the existing
// `disposeNewDiscardOwnedOutSlots` loop still drives it).
//
// The throw-site CALLEE exception path is the focus of this test; the
// `out owned var x` (real binding) sibling is already covered —
// ensureOutVarSlots registers an `unwindOnly` entry for the inline
// recipient, so the throw-after-assign repro there is sound.
//
// FAILS-WITHOUT / PASSES-WITH.
//   * runAotChecked (ASan/LSan): pre-fix `r.ran` is FALSE (LSan Direct
//     leak at exit). With the fix the run completes cleanly and
//     `Res.alive` settles at 0 after the throw is caught (one ctor
//     mint, one dispose via the unwinder's null-guarded dynamicDispose).
//   * The nested-in-a-method-call CONTROL (`M(out _)` shape) confirms
//     the #1467-reachable case is unaffected.
//   * The normal-return CONTROL (`Make(out _)`) confirms the existing
//     `discardOwnedOutSlots_` normal-path disposal still fires.
//
// per §22.4, §28.3, §17.13, #1833 (a), #1467

#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Res — owned instance, destructor decrements a static counter we read
// after disposal (sanity test: at scope end, Res.alive == 0 → exactly-once
// disposal of every constructor-minted Res, even on the throw path).
const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; Res.alive := Res.alive + 1; "
    "    end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// (1) §22.4 / §28.3 — `out _` against `out owned` where the CALLEE ASSIGNS
// then THROWS. The throw edge must dispose the just-written scratch
// exactly once. Pre-fix LSan flags a Direct leak in `bz$W$TryMakeThrow`;
// post-fix r.ran is true and Res.alive settles at 0 (1 mint + 1 dispose
// on the unwind).
// ===========================================================================
BEZEL_TEST(owned_out_discard_throw_unwind_1833, callee_assigns_then_throws_dispose_on_unwind_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class W "
        "  public static method TryMakeThrow(out owned r: Res?): bool "
        "    r := new Res(9); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end TryMakeThrow; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    try "
        "      var ok := TryMakeThrow(out _); "   // target: throw-after-assign
        "      return -1; "                       // unreachable; throws first
        "    catch "
        "      var e: Exception => null; "
        "    end; "
        "    return Res.alive; "                  // 0 → mint +1 → dispose -1 = 0
        "  end Main; "
        "end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LSan Direct leak flags ran=false
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) §22.4 / §28.3 — CONTROL: normal-return path `Make(out _)`. The
// `discardOwnedOutSlots_` loop (the #1467 fix) handles this; the new
// `unwindOnly` entry must NOT double-dispose (it carries `unwindOnly=true`
// so the normal path skips it). Res.alive settles at 0 post-call.
// ===========================================================================
BEZEL_TEST(owned_out_discard_throw_unwind_1833, normal_return_path_control_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class W "
        "  public static method Make(out owned r: Res?): bool "
        "    r := new Res(5); "
        "    return true; "
        "  end Make; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var ok := Make(out _); "             // normal-return path
        "    return Res.alive; "                  // 0 → +1 -1 = 0
        "  end Main; "
        "end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §22.4 / §28.3 — CONTROL: `TryMakeThrow(out _)` *nested* in another
// call's argument list — `Sink(TryMakeThrow(out _))`. Confirms the
// nested-in-a-method-call shape (the #1467 path) handles the throw
// correctly via the enclosing call's processing. Distinct from the
// top-level `out _` repro because the enclosing call's
// `discardOwnedOutSlots_` walk does its own unwind bookkeeping.
// ===========================================================================
BEZEL_TEST(owned_out_discard_throw_unwind_1833, nested_in_call_throw_path_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class W "
        "  public static method TryMakeThrow(out owned r: Res?): bool "
        "    r := new Res(11); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end TryMakeThrow; "
        "  public static method Sink(x: bool): int => 0; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var ok := 0; "
        "    try "
        "      ok := Sink(TryMakeThrow(out _)); "
        "      return -1; "
        "    catch "
        "      var e: Exception => null; "
        "    end; "
        "    return Res.alive; "                  // 0 → +1 (mint) -1 (dispose) = 0
        "  end Main; "
        "end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}