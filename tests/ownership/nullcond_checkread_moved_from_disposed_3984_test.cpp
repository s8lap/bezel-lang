// fix #3984 (§28.3 K1.3/K7, §28.2, v0.295): the residual half of #3971.
// `MethodOwnership::checkRead` had no arm for `NullCondMemberExpr` (`t?.M`)
// or `NullCondCallExpr` (`f?(args)`), so a read of an already moved-from (or
// explicitly disposed) owned local through `?.` was undiagnosed and the
// accepted program heap-use-after-frees. Landed once its blocker, issue
// 3983 (a nullable owned local's plain null-CHECK falsely arming
// `disposedOnSomePath`), was fixed — adding these arms before that would
// have multiplied 3983's false diagnostic onto the canonical
// `item?.Dispose()` discharge idiom (owned_collection_trypop_831,
// owned_set_take_1476).
//
// Measured on origin/main + the #3971 fix (pre-#3984): `var xn: Res? := new
// Res(1); var t := Sink.Take(move xn!); var u := xn?.tag;` compiled with
// rc=0 and zero diagnostics and heap-use-after-freed under
// `--sanitize=address` ("READ of size 4" freed in `bz$M.Sink$Take`);
// `xn?.Get()` behaved identically. The control `Sink.Peek(xn!)` in the same
// position was already correctly rejected — the only difference was whether
// the read sat behind `?.`.
//
// A second, smaller fix rides along: `handleDischarge`'s
// `skipCalleeReadForDispose` (Ownership_Checks.cpp, near the callee-read
// gate above the argument-mode loop) recognised only a MemberAccessExpr
// callee, so once the NullCondMemberExpr arm existed, `x?.Dispose()` on an
// already-disposed `x` got checkRead's generic §28.2 use-after-dispose
// WARNING (which POISONS the record) instead of markDisposedByName's
// precise "is disposed again here" message — the #3941 poison-preemption
// shape again, now for the `?.` spelling. Widened to read the callee's
// (target, member) pair from EITHER member-access node kind.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kResSinkPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  public method Get(): int return this.tag; end Get; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int "
    "    r.Dispose(); return r.tag; "
    "  end Take; "
    "end Sink; ";

}  // namespace

// The issue's exact measured repro: `xn?.tag` reading a moved-from owned
// local. Was accepted (rc=0, zero diagnostics) and heap-use-after-freed
// pre-fix; must now be REJECTED with the same precise message the direct
// (unaliased) `Sink.Peek(xn!)` control already got.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           nullcond_member_read_after_move_rejected_spec_28_3) {
    auto r = runAot(std::string(kResSinkPrelude) +
        "class C public static method Main(): int "
        "  var xn: Res? := new Res(1); "
        "  var t := Sink.Take(move xn!); "
        "  var u := xn?.tag; "
        "  return t + (u ?? 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `xn` was already moved out at this point"));
}

// The issue's second spelling: `xn?.Get()` (a NullCondCallExpr whose callee
// is the NullCondMemberExpr) — the issue records this "behaves identically".
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           nullcond_call_read_after_move_rejected_spec_28_3) {
    auto r = runAot(std::string(kResSinkPrelude) +
        "class C public static method Main(): int "
        "  var xn: Res? := new Res(1); "
        "  var t := Sink.Take(move xn!); "
        "  var u := xn?.Get(); "
        "  return t + (u ?? 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `xn` was already moved out at this point"));
}

// A `?.Dispose()` on a moved-from local is ALSO a read of the (freed)
// receiver — must be rejected the same way, not silently accepted as a
// no-op null-conditional dispose.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           nullcond_dispose_after_move_rejected_spec_28_3) {
    auto r = runAot(
        "module M; "
        "class Res public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink public static method Take(owned r: Res): int "
        "  return r.tag; "
        "end Take; end Sink; "
        "class C public static method Main(): int "
        "  var xn: Res? := new Res(1); "
        "  var t := Sink.Take(move xn!); "
        "  xn?.Dispose(); "
        "  return t; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `xn` was already moved out at this point"));
}

// fix #3984 part 2: a double `?.Dispose()` must still get markDisposedByName's
// PRECISE idempotent-dispose message, not checkRead's generic (record-
// poisoning) use-after-dispose warning. Both are diagnostics (Warning
// severity — disposal is idempotent per §28.2, so the program still runs),
// but the precise one must be the one that shows up, and the generic one
// must NOT also fire for the same site.
//
// Uses `parseAndCheckOwnership` (sema/ownership only, not the full AOT
// pipeline) rather than `runAot`: this diagnostic is Warning-severity, so
// the program still compiles, and `runAot` DISCARDS captured compiler
// stderr on a successful compile (aot_support.hpp) — scanning `r.diagnostics`
// after a `runAot` success would be vacuous (LANE_RULES / tests/README.md).
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           nullcond_double_dispose_keeps_precise_message_spec_28_2) {
    auto r = bezel::test::parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C public method M() "
        "  var xn: Res? := new Res(); "
        "  xn?.Dispose(); "
        "  xn?.Dispose(); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool precise = false;
    bool generic = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is disposed again here") != std::string::npos)
            precise = true;
        if (d.message.find("after a dominating explicit Dispose() call") !=
            std::string::npos)
            generic = true;
    }
    BEZEL_EXPECT_TRUE(precise);
    BEZEL_EXPECT_FALSE(generic);
}

// ACCEPT-SIDE CONTROL (must stay green — issue 3983's own required oracle):
// the canonical `out owned var` TryPop discharge idiom — a null-check read
// through `.` (not `?.`) followed by an idempotent `?.Dispose()` — must
// still compile and run correctly with these arms in place. Mirrors
// owned_collection_trypop_831.array_try_pop_first_true.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           trypop_null_check_then_qdispose_control_accepted_spec_28_2) {
    auto r = runAot(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    a.Add(move (new Box(10))); a.Add(move (new Box(20))); "
        "    var ok := a.TryPopFirst(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ACCEPT-SIDE CONTROL: an ordinary live (never moved, never disposed) owned
// nullable local read through `?.` must stay silent — the new arms are a
// pure read-walk and must not reject a legitimate live read.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           live_owned_local_qmember_read_control_accepted_spec_28_3) {
    auto r = runAot(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var item: Box? := new Box(42); "
        "  var v := item?.Val; "
        "  item?.Dispose(); "
        "  return v ?? 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ============================================================================
// fix-forward #3842 (§22.4 "Inline `out` declarations" / §28.11, stdlib §1
// shape (a), v0.334): the `out var` BUILTIN extraction table in
// sema/ExprTypecheck_Exprs.cpp carried no arm for the owning-collection
// Try's whose out param the Stack/Queue arms did not match, although the
// `out owned var` and `out using var` tables beside it have carried those
// arms since #831/#1354. §28.11 derives the inline recipient's type from the
// RECEIVER's element argument, so with no arm the recipient is left UNTYPED
// and every read through it fails to lower.
//
// #3842 (8dd17f3b) RETIRED the `out owned var` call-site spelling
// (§A.4, v0.289 L2) and migrated this file's own accept-side control from
// `out owned var item` to `out var item` — which is what made the gap
// reachable. MEASURED at 8dd17f3b, `OwningArray<Box>.TryPopFirst(out var
// item)` compiled rc=0, RAN, and returned 0 instead of 10, with
//   "assignment RHS expression failed to lower to a value for this
//    assignment's target (store dropped …) — #2961"
// because `item.Val` had no type to lower through. Same for
// OwningLinkedList.TryPopFirst and OwningDictionary.TryPopValue;
// OwningStack.TryPop was already covered by the Stack/Queue arm and
// answered 5.
//
// NOT the substitution condition: the `substituteMemberType` block at the
// top of the same handler is unreachable here — a BUILTIN Try* callee
// resolves to no MethodSymbol, so `formalOutParamType2` returns null and
// `calleeParent2` is null. MEASURED: reverting ExprTypecheck_Exprs.cpp
// wholesale to the parent leaves this repro failing identically.
//
// These rows pin the VALUE that comes back out of the discharge idiom, so a
// fix that merely silenced the #2961 warning (typed the recipient `int`, say)
// still fails them.
// ============================================================================
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           owningarray_trypopfirst_out_var_recipient_typed_spec_28_11) {
    auto r = runAot(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    a.Add(move (new Box(10))); a.Add(move (new Box(20))); "
        "    var ok := a.TryPopFirst(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// The OwningLinkedList twin of the row above (stdlib §4.7) — same table, same
// missing arm. A fix that added only the `OwningArray` arm leaves this red.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           owninglinkedlist_trypopfirst_out_var_recipient_typed_spec_28_11) {
    auto r = runAot(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningLinkedList<Box>(); "
        "    a.Add(move (new Box(10))); a.Add(move (new Box(20))); "
        "    var ok := a.TryPopFirst(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// OwningDictionary.TryPopValue (stdlib §4.17, #860): a KEYED extraction, so its out
// param is at position 1 and its type is the VALUE argument genericArgs[1] —
// a different slot AND a different position from the TryGetValue arm the table
// already had. Exit 7, not 10, so this cannot pass by reusing the element arm.
BEZEL_TEST(nullcond_checkread_moved_from_disposed_3984,
           owningdictionary_trypopvalue_out_var_recipient_typed_spec_28_11) {
    auto r = runAot(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Box>(); "
        "    d.Add(\"k\", move (new Box(7))); "
        "    var ok := d.TryPopValue(\"k\", out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
