// fix #1833 (b) / §22.4 / §28.3 / §17.13 (spec v0.214, v0.215): the
// constructor-call `out _` against an `out owned` ctor parameter at a
// TOP-LEVEL / statement-context constructor call site (`new R(out _);`)
// was recorded by buildOrderedArgs but NEVER disposed by lowerNew —
// strictly parallel to the #1467 method-call fix, which lowerCall already
// drives via the discardOwnedOutSlots_ machinery. The nested-in-a-call case
// (`M(new R(out _))`) is incidentally disposed by the enclosing
// lowerCall's loop (no leak, no double-free), but the top-level case
// never reached a disposal slot and the produced owned value leaked
// (LSan: "Direct leak ... bz$R$ctor").
//
// The fix: lowerNew processes its OWN additions to discardOwnedOutSlots_
// the same way lowerCall does — null-guard the scratch, dispatch vtable
// Dispose + reclaim, exactly once — on the normal-return path
// (`!diverged_`). The shape of the disposal loop matches lowerCall's so
// the emit policy is identical between call sites (no semantic drift
// between `out _` against a method's `out owned` and `out _` against a
// ctor's `out owned`).
//
// FAILS-WITHOUT / PASSES-WITH.
//   * runAotChecked (ASan/LSan): r.ran is FALSE pre-fix (LSan Direct
//     leak detected at exit; the ctor's `bz$R$ctor` symbol name — the
//     discarded owned slot written by `o := new Res(7)` — never reached
//     a dispose). WITH the fix the run completes cleanly and the
//     destructor fires exactly once (post-dispose destructor counter
//     settles at 0, "ok" prints).
//   * The nested-in-a-call CONTROL confirms the #1467-reachable case is
//     unaffected: counts match.
//
// per §22.4, §28.3, §17.13, §28.10, #1833 (b), #1467

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Res — owned instance, destructor decrements a static counter we read
// after disposal (sanity test: at scope end, Res.alive == 0 → exactly-once
// disposal of every constructor-minted Res).
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
// (1) §22.4 / §28.3 — `new R(out _)` at the top-level / statement-context
// site: the discarded owned Res disposed exactly once. Pre-fix LSan
// reports a Direct leak (the recorded discard slot was never disposed).
// Post-fix r.ran is true and Res.alive settles at 0 after scope exit.
//
// The ctor body writes `o := new Res(7)` and does NOT re-store `o` into
// an owned field of `R`. That mirrors the issue #1833 (b) repro
// verbatim. Reading `Res.alive` post-construction confirms the
// discarded Res is gone: each ctor mint → +1; the discard slot dispose
// → -1; net 0.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_discard_top_level_1833, new_r_out_discarded_at_stmt_disposed_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class R "
        "  public constructor(out owned o: Res?) o := new Res(7); "
        "    end constructor; "
        "end R; "
        "class W public static method Main(): int "
        "  Res.alive := 0; "
        "  var q := new R(out _); "      // top-level ctor-discard (the test target)
        "  return Res.alive; "           // 0 → +1 (ctor mint) -1 (dispose) = 0
        "  end Main; end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LSan Direct leak flags ran=false
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) §22.4 / §28.3 — NESTED in a method call: `Sink(new R(out _))` is
// incidentally disposed by the enclosing lowerCall (the #1467 path).
// Control: confirms the lowerNew fix doesn't change that behavior or
// introduce a double-dispose at the inner ctor site.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_discard_top_level_1833, new_r_out_discarded_nested_control_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class R "
        "  public constructor(out owned o: Res?) o := new Res(7); "
        "    end constructor; "
        "end R; "
        "class W "
        "  public static method Sink(r: R): int => 0; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var v := Sink(new R(out _)); "   // nested — #1467 path
        "    return Res.alive; "
        "  end Main; "
        "end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §22.4 / §28.3 — `out _` against a CLASS-typed `out owned` (not
// nullable) at the top-level: the dispose path gates on class/interface
// declaration. Pins the policy with the §22.4-conformant non-nullable
// alternative.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_discard_top_level_1833, new_r_out_non_nullable_disposed_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class R "
        "  public constructor(out owned o: Res) o := new Res(5); "
        "    end constructor; "
        "end R; "
        "class W public static method Main(): int "
        "  Res.alive := 0; "
        "  var q := new R(out _); "
        "  return Res.alive; "
        "end Main; end W;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

