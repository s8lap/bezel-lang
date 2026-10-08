// fix #1918 — §28.3 "Exception-path discharge", spec v0.197. Branch/path-
// sensitive exception-unwind disposal of a plain `owned` local that is DISCHARGED
// on ONE branch via an OWNED-ARGUMENT TRANSFER but LIVE at a `throw` on a SIBLING
// branch. Codegen-observable, LeakSanitizer/AddressSanitizer-checked.
//
// The defect: collectUnwindIneligible's UnwindIneligibleWalker
// (LlvmBackend_Setup.cpp) was BRANCH-INSENSITIVE — a consume ANYWHERE in the body
// (here `Sink(owned r)` on the if-branch) tainted `r` as unwind-INELIGIBLE for
// the WHOLE body, so registerUnwindOwnedLocal registered no `unwindOnly`
// disposal. On the SIBLING throw branch, where `r` is still live, nothing
// disposed it → a 16-byte leak on the throw edge (§28.3 requires exactly-once
// discharge on every path). Pre-existing and NOT state-machine-specific: the
// synchronous method leaked identically to the async one.
//
// The fix (two coordinated codegen sites):
//   * UnwindIneligibleWalker (LlvmBackend_Setup.cpp) no longer taints a bare
//     owned LOCAL / owned PARAMETER passed with an ownership TRANSFER (explicit
//     `owned` argument, or a mapped `owned` callee parameter) to an ORDINARY
//     user method — so registerUnwindOwnedLocal/Param leaves it ELIGIBLE.
//   * buildOrderedArgs (LlvmBackend_Assign.cpp, `nullTransferredOwned` — set only
//     on that user-method call path) NULLS the caller's slot for that argument at
//     the "call about to run" point. The unwindOnly disposal is null-guarded, so
//     it is now EXACTLY-ONCE across branches:
//       - the transferred value's slot is null on the transfer / post-transfer
//         throw edge → the caller's disposal skips it (no DOUBLE-FREE with the
//         callee's own owned-param unwind disposal);
//       - a sibling-branch value still live at a throw keeps a non-null slot →
//         disposed once (no leak).
//   Mirrors how an explicit `x.Dispose()` (#865) / `_ := x` discard (#1893)
//   already null the slot for the same null-guarded machinery.
//
// Observation model (as in exception_unwind_frame_local_1896_1897_test): class
// Resource bumps a static net-live counter (ctor +1, dtor -1). Disposed exactly
// once => net 0; a leak => +1; a double-free => -1 AND an ASan trip under the
// runAotChecked oracle. Each leg rides runAotChecked (AOT + ASan/LSan), so
// r.ran==false flags any leak/double-free/UAF and the destructor-count exit code
// is asserted together. The critical double-free guard is the transfer-window
// leg, where the consumer TAKES ownership then THROWS: the callee disposes the
// owned param on its unwind, and the caller must NOT also dispose it.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Resource: static net-live counter. Disposed exactly once => net 0; leaked =>
// +1; double-disposed => -1 (and the second reclaim trips ASan under the check).
const char* kResource =
    "class Resource "
    "  public static _live: int := 0; "
    "  public constructor() Resource._live := Resource._live + 1; end constructor; "
    "  destructor Resource._live := Resource._live - 1; end destructor; "
    "end Resource; ";

}  // namespace

// ===========================================================================
// #1918 CORE (synchronous, §28.3) — `r` is discharged on the if-branch via an
// owned-argument transfer `Sink(owned r)` and LIVE at a `throw` on the else
// branch. On the (taken) throw branch it must be unwind-disposed exactly once.
// FAIL-WITHOUT: r leaks in bz$Program$Work — r.ran==false (LSan); no-ASan value
// would be 11 (caught=1, live=1). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, sync_owned_arg_discharged_one_branch_live_throw_sibling_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink(owned r: Resource) "
        "    _ := r; "                              // announced-consume discharge
        "  end Sink; "
        "  public static method Work(flag: bool) "
        "    var r := new Resource(); "             // plain owned local
        "    if flag then "
        "      Sink(move r); "                     // discharged on THIS branch
        "    else "
        "      throw new InvalidOperationException(\"boom\"); "  // r LIVE here (§28.3)
        "    end; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try "
        "      Work(false); "                       // take the throwing branch
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "  // 10 = caught & disposed once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // caught == 1 AND Resource._live == 0
}

// ===========================================================================
// #1918 CORE (async, §28.3 / §30.1) — the same branch-sensitive shape in a
// state-machine frame: `r` is frame-spilled across an await, discharged on the
// if-branch via `Sink(owned r)`, and LIVE at a throw on the else branch.
// FAIL-WITHOUT: r leaks in bz$async$Work$Resume. PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, async_owned_arg_discharged_one_branch_live_throw_sibling_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink(owned r: Resource) "
        "    _ := r; "
        "  end Sink; "
        "  public static async method Work(flag: bool): Task "
        "    var r := new Resource(); "
        "    await Task.Delay(0); "                 // suspend: r is frame-spilled
        "    if flag then "
        "      Sink(move r); "
        "    else "
        "      throw new InvalidOperationException(\"boom\"); "
        "    end; "
        "  end Work; "
        "  public static async method Run(): Task<int> "
        "    var caught := 0; "
        "    try "
        "      await Work(false); "
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await Run(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1918 DOUBLE-FREE GUARD (the critical soundness leg, §28.3) — the consumer
// `ConsumeThenThrow(owned r)` TAKES ownership and then THROWS. On that transfer-
// window edge the CALLEE owns `r` and disposes it on its owned-param unwind; the
// caller must NOT also dispose it (that would be a double-free). The fix nulls
// the caller's slot at the transfer, so the caller's null-guarded disposal skips
// it. Legs: (1) flag=true exercises the transfer-window; (2) flag=false exercises
// the sibling live-throw. Net Resource._live must be 0 across both.
// FAIL-WITHOUT: leg (2) leaks in bz$Program$Work (value 201). If the fix
// double-freed leg (1), ASan trips (r.ran==false) or the value is 199.
// PASS-WITH: exit == 200 AND r.ran (no leak, no double-free).
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, transfer_window_consumer_throws_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method ConsumeThenThrow(owned r: Resource) "
        "    throw new InvalidOperationException(\"in consume\"); "  // callee owns r here
        "  end ConsumeThenThrow; "
        "  public static method Work(flag: bool) "
        "    var r := new Resource(); "
        "    if flag then "
        "      ConsumeThenThrow(move r); "         // transfer-window: callee throws
        "    else "
        "      throw new InvalidOperationException(\"boom\"); "  // sibling: r live
        "    end; "
        "  end Work; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    try "
        "      Work(true); "                         // callee takes ownership then throws
        "    catch var e: InvalidOperationException => "
        "      n := n + 1; "
        "    end; "
        "    try "
        "      Work(false); "                        // sibling live throw
        "    catch var e: InvalidOperationException => "
        "      n := n + 1; "
        "    end; "
        "    return n * 100 + Resource._live; "      // 200 = both caught, disposed once each
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free on the transfer-window edge
    BEZEL_EXPECT_EQ(r.exitCode, 200);  // n == 2 AND Resource._live == 0
}

// ===========================================================================
// #1918 — an OWNED PARAMETER re-passed onward as an owned argument. `Relay(owned
// r)` forwards its owned param to `Sink(owned r)`; on the sibling branch the
// param is live at a throw and must be unwind-disposed exactly once. Exercises
// the ParameterSymbol arm of the walker/nulling (registerUnwindOwnedParam).
// FAIL-WITHOUT: r leaks in bz$Program$RelayCond. PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, owned_param_repassed_live_throw_sibling_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink(owned r: Resource) "
        "    _ := r; "
        "  end Sink; "
        "  public static method Relay(owned r: Resource) "
        "    Sink(move r); "                        // owned param re-passed onward
        "  end Relay; "
        "  public static method RelayCond(flag: bool) "
        "    var r := new Resource(); "
        "    if flag then "
        "      Relay(move r); "
        "    else "
        "      throw new InvalidOperationException(\"boom\"); "
        "    end; "
        "  end RelayCond; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try "
        "      RelayCond(false); "
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1918 — the ORPHAN edge (§28.3, adjacent to #993): a LATER argument throws
// while evaluating the argument list, BEFORE the call runs. `r` was already
// evaluated into the first (owned) slot but ownership has NOT transferred (the
// callee never ran), so `r` must be disposed on that unwind edge. The slot-nulling
// happens only AFTER the whole list is evaluated, so it does not fire on this
// edge and the live `r` is correctly reclaimed.
// FAIL-WITHOUT: r leaks in bz$Program$OrphanArg. PASS-WITH: exit == 0.
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, later_arg_throws_owned_arg_orphan_disposed_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink2(owned r: Resource, n: int) "
        "    _ := r; "
        "  end Sink2; "
        "  public static method MayThrow(): int "
        "    throw new InvalidOperationException(\"x\"); "
        "  end MayThrow; "
        "  public static method OrphanArg() "
        "    var r := new Resource(); "
        "    Sink2(move r, MayThrow()); "           // MayThrow throws before the call
        "  end OrphanArg; "
        "  public static method Main(): int "
        "    try "
        "      OrphanArg(); "
        "    catch var e: InvalidOperationException => "
        "      _ := e; "
        "    end; "
        "    return Resource._live; "                // 0 = r disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1918 CONTROL (§28.3) — the NORMAL path: the owned-argument transfer branch is
// taken and completes without throwing. `Sink(owned r)` discharges `r` exactly
// once (announced consume); the new unwind-eligibility must NOT cause a second
// disposal on the normal edge (the unwindOnly disposal is isUnwind-gated and the
// slot is nulled at the transfer). Net Resource._live == 0. Passes with and
// without the fix — guards against a normal-path double-free regression.
// ===========================================================================
BEZEL_TEST(unwind_branch_owned_arg_transfer_1918, normal_path_owned_arg_transfer_not_double_freed_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink(owned r: Resource) "
        "    _ := r; "
        "  end Sink; "
        "  public static method Work(flag: bool) "
        "    var r := new Resource(); "
        "    if flag then "
        "      Sink(move r); "
        "    else "
        "      throw new InvalidOperationException(\"boom\"); "
        "    end; "
        "  end Work; "
        "  public static method Main(): int "
        "    Work(true); "                           // normal completion (no throw)
        "    return Resource._live; "                // 0 = disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
