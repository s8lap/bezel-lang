// Codegen — issue #4422: calling a reassigned closure local (`return f()`)
// leaks 40 B per call. Measured on the current compiler (`bezelc
// --sanitize=address`, LeakSanitizer on, 500x amplified):
//
//     var f: () -> int := lambda() => 0;
//     f := lambda() => 42;
//     return f();    // ← calling the closure leaks 40 B per call
//
// `return f` (returning the closure, not calling it) is fine — the closure
// is transferred to the caller and the caller's take-ownership releases it.
// But `return f()` calls the closure and returns an int, leaving the closure
// in the slot with no release at scope end (a reassigned local's final value
// is excluded from scope-end release by ClosureEscapeWalker's own design).
//
// ROOT CAUSE. The LocalSymbol arm in `emitClosureCall` (LlvmBackend_Calls2.cpp
// ~6501) loads the closure from the slot and calls it, but emits NO release
// after the call. For a non-reassigned closure local, the scope-end release
// fires and the closure is freed. For a reassigned local, the scope-end
// release is suppressed (the reassigned local is marked as escaped by
// ClosureEscapeWalker), so the closure is never freed: a leak.
//
// FIX. Emit `emitClosureRelease(closurePtr)` after `emitClosureCall` returns
// in the LocalSymbol arm, mirroring `lowerClosureLocalReassign`'s own
// retain-before-store / release-old discipline for the reassignment path.
//
// The three FAIL-WITHOUT / PASS-WITH rows below are direct siblings of the
// three rows in `closure_local_reassign_release_4174_test.cpp` (which covers
// the `return f` path), scaled to 500 calls so the 40 B/call leak clears
// LSan reporting floors. The two-iteration row is the two-reassignment
// sibling of `reassigned_closure_local_two_reassignments_call_releases_both`.
//
// per issue #4422 (see the CITATION NOTE in closure_local_reassign_release_4174_test.cpp
// for why no spec section is claimed as authority for this mechanism)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact repro, 500x amplified in a loop so the 40-byte
// per-call leak clears LeakSanitizer's reporting floor (a one-shot can pass
// on both sides — LANE_RULES §3). Pre-fix: 20000 bytes / 500 allocations
// leaked (the final closure value, called but never released). Post-fix:
// clean. The returned value must still be correct (42) — the release must
// not disturb the call's result. (issue #4422)
BEZEL_TEST(closure_local_reassign_call_release_4422,
           reassigned_closure_local_called_per_call_release_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int := lambda() => 0; "
        "    f := lambda() => 42; "
        "    return f(); "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 42 = 21000; 21000 mod 97 = 3 */ 21000 % 97);
}

// ── SCALING axis: two reassignments displace TWO distinct values, so TWO
// leaks per call. Pre-fix: 40000 bytes / 1000 allocations leaked (80 B/call).
// Post-fix: clean. (issue #4422)
BEZEL_TEST(closure_local_reassign_call_release_4422,
           two_reassignments_called_both_displaced_values_released_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int := lambda() => 0; "
        "    f := lambda() => 1; "
        "    f := lambda() => 42; "
        "    return f(); "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 42 = 21000; 21000 mod 97 = 3 */ 21000 % 97);
}

// ── PARITY CONTROL: non-reassigned closure local called in a loop is already
// clean on the pre-fix compiler and must stay clean. This is NOT a
// FAIL-WITHOUT row — it is a regression guard ensuring the fix does not
// disturb the non-reassigned path. (issue #4422)
BEZEL_TEST(closure_local_reassign_call_release_4422,
           non_reassigned_closure_local_still_clean_parity_spec_27_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int := lambda() => 42; "
        "    return f(); "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;"),
        /* 500 * 42 = 21000; 21000 mod 97 = 3 */ 21000 % 97);
}
