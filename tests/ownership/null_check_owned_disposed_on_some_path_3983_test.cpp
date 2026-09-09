// fix #3983 (§28.3 K1 polymorphic-null / K7.4, v0.295): a nullable owned
// local's plain null-CHECK — `if item <> null then ... end;` with no
// discharge on either arm — used to arm `disposedOnSomePath` at the
// if-statement's merge, so EVERY later read of `item` drew the false
// diagnostic "local `item` may have already been disposed on another
// branch and may not be read here (conditional-discharge join, §28.3 /
// §28.2 ...)".
//
// Root cause: `MethodOwnership::run`'s CFG dataflow (Ownership.cpp
// computeEntry/foldPred, the `nullEdgeName` mechanism added for #764) feeds
// a provably-null local into the false/true "null edge" of an `x <> null`
// / `x = null` branch as OwnershipState::Disposed — modelling "disposal of
// null is a no-op". At the merge, `joinInto`'s `ownedDischargedSplit` arm
// (Ownership_Checks.cpp) cannot tell that manufactured Disposed apart from
// a REAL one-branch `.Dispose()` call, so it armed `disposedOnSomePath`
// (§1883) even though nothing was ever disposed on the null path — there
// was no object there to dispose.
//
// The fix feeds PolyNull (K1's "no obligation established on this path"
// state — already used by the sibling `out owned var` TryPop/#1407 false-
// edge demotion right below the same call site) instead of Disposed.
// PolyNull's `joinInto` arm is pure absorption (`PolyNull (+) X = X`, no
// flag touched), so:
//   * a then-arm that stays Owned (an ordinary read, no discharge) merges
//     to plain Owned, same as before — no disposedOnSomePath, no false
//     reject;
//   * a then-arm that explicitly disposes still merges to Disposed, same
//     as before (Disposed (+) Disposed, or PolyNull (+) Disposed = Disposed
//     either way) — behaviour-preserving;
//   * a REAL one-branch dispose on a NON-null-check condition (the #1883
//     genuine soundness hole `nullEdgeName` never touches) still arms
//     disposedOnSomePath and is still rejected — see
//     `genuine_one_branch_dispose_still_rejected_control_spec_28_3` below,
//     which is a deliberate control that passes IDENTICALLY before and
//     after this fix (it never goes through `nullEdgeName` at all).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kBoxPrelude =
    "module M; "
    "class Box public Val: int := 0; "
    "  public constructor(v: int) Val := v; end constructor; end Box; ";

}  // namespace

// The issue's exact measured repro (#3983): a plain `<> null` guard with a
// read in the true arm, then a further read (`item!.Val`) and an idempotent
// `item?.Dispose()` past the merge. Was rejected with the false
// disposed-on-some-path diagnostic on origin/main pre-fix; now compiles and
// returns v(10) + w(10) == 20.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           null_check_then_read_accepted_spec_28_3) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box? := new Box(10); "
        "  var v := 0; if item <> null then v := item.Val; end; "
        "  var w := item!.Val; "
        "  item?.Dispose(); "
        "  return v + w; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// §28.2 equivalence the issue calls load-bearing: `item!.Dispose()` (a bang
// force-unwrap) must be accepted exactly where `item?.Dispose()` is — the
// discharge idiom is not special-cased to one spelling of the receiver.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           null_check_then_bang_dispose_equivalence_spec_28_2) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box? := new Box(10); "
        "  var v := 0; if item <> null then v := item.Val; end; "
        "  var w := item!.Val; "
        "  item!.Dispose(); "
        "  return v + w; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// The reversed-operand spelling `null <> item` is recognised by the same
// `nullEdgeName` machinery (per #764) and must get the same fix.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           reversed_null_operand_accepted_spec_28_7) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box? := new Box(10); "
        "  var v := 0; if null <> item then v := item.Val; end; "
        "  var w := item!.Val; "
        "  item?.Dispose(); "
        "  return v + w; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// The `= null` spelling (null on the TRUE edge instead of the false edge)
// with an explicit else arm — the read sits in the else arm this time.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           equals_null_else_form_accepted_spec_28_3) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box? := new Box(10); "
        "  var v := 0; if item = null then v := 0; else v := item.Val; end; "
        "  var w := item!.Val; "
        "  item?.Dispose(); "
        "  return v + w; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// Two SEPARATE null-checks of the same local, each followed by its own
// post-merge read, plus a final `!`-read and dispose — pins that the fix
// is not a one-shot special case but holds across repeated merges of the
// same symbol.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           repeated_null_checks_all_reads_accepted_spec_28_3) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box? := new Box(10); "
        "  var a := 0; if item <> null then a := item.Val; end; "
        "  var b := 0; if item <> null then b := item.Val; end; "
        "  var c := item!.Val; "
        "  item?.Dispose(); "
        "  return a + b + c; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// CONTROL, passes identically before and after this fix (§28.3 #1883's
// genuine soundness hole, which `nullEdgeName` never touches because the
// condition here is a bare bool, not an `x <> null` / `x = null` test): a
// REAL one-branch `.Dispose()` still must arm disposedOnSomePath and still
// must reject a later read. This is the anti-regression pin that the fix
// narrowed the false-positive WITHOUT widening the accept surface for the
// genuine use-after-dispose danger.
BEZEL_TEST(null_check_owned_disposed_on_some_path_3983,
           genuine_one_branch_dispose_still_rejected_control_spec_28_3) {
    auto r = runAot(std::string(kBoxPrelude) +
        "class C public static method Main(): int "
        "  var item: Box := new Box(10); "
        "  var flag := true; "
        "  if flag then item.Dispose(); end; "
        "  var w := item.Val; "
        "  return w; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may have already been disposed on another branch"));
}
