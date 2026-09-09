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
        "    var ok := a.TryPopFirst(out owned var item); "
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
