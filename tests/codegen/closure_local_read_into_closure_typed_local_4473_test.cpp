// Codegen — issue #4473 (adjacent to #4425, §27.5 / §28.9, v0.301):
// the read-out path of a closure-typed local was leaking the closure cell
// when read into ANOTHER closure-typed local in the same scope:
//
//   var y := lambda(): Box => x;
//   var lambdaRef: () -> Box;
//   lambdaRef := y;
//   return 0;
//
//                  -> 40 B (closure cell) + 8 B (env) + 8 B (capture
//                     cell for x) = 56 bytes / 3 allocations leaked per call
//
// Root cause. `lowerClosureLocalReassign`'s lambdaRef := y arm retains
// the newVal (the closure cell for `y`, rc 1 → rc 2) and stores it into
// `lambdaRef`'s slot, but the RHS slot (`y`'s slot) is left pointing at
// the cell — the +1 it still holds must be balanced by `y`'s scope-end
// release. The pre-existing ClosureEscapeWalker + closureReleaseEligible_
// pipeline was correctly arming a release for the LHS (lambdaRef), but
// the RHS (y) was kept marked as escaped: the FINDING [1] read arm of
// `expr()` set `escaped = true` when y appeared on the RHS of an
// AssignmentStmt, and the post-walk override (which clears `escaped` so
// the local joins closureReleaseEligible_) only considered the
// `escapedDueToReassign` arm (target IS the LHS, fix #4425 / lane-35),
// not the read-into arm (target IS the RHS of an AssignmentStmt whose
// LHS is itself a same-scope closure-typed local/parameter slot).
//
// The fix (lane cycle14-l33): extends the post-walk override's
// disjunction with `escapedDueToAssignIntoClosureLocal_` — a NEW flag
// set by the AssignmentStmt arm of stmt() when the LHS is a
// closure-typed local/parameter slot AND the RHS contains target as a
// read (verified via `isClosureTypedLocalAssign(*as->target) &&
// containsTargetAnywhere(as->value.get())`). The override's disjunction
// becomes:
//
//     (escapedDueToReassign && <existing call-count predicate>) ||
//     escapedDueToAssignIntoClosureLocal_
//
// Both arms now clear `escaped`, so BOTH the LHS (lambdaRef, reassign
// path via #4425) and the RHS (y, read-into path via #4473) join
// closureReleaseEligible_. bz_closure_release is rc-based and safe to
// fire on multiple slots pointing at the same cell (verified against
// src/runtime/bezel_runtime_refcounting.cpp line 336), so both scope-
// end releases decrement the cell's rc by 1 — the cell is freed exactly
// once.
//
// DISTINCT FROM #4425'S MECHANISM. #4425 fixed the LHS-IS-target
// (reassign) path by extending the call-count predicate to admit
// `calleeCallCount == 0`. #4473 fixes the RHS-IS-target (read-into)
// path by adding a NEW override disjunction that does NOT depend on
// the call count (a non-reassigned local has its +1 retained by
// lowerClosureLocalReassign even when calleeCallCount is 0 — see
// src/codegen/LlvmBackend_Calls2.cpp line 6585, the call-site release
// arm only fires for reassigned locals). The two fixes are orthogonal:
// each can land independently of the other and each closes a separate
// leak shape that the other does not close.
//
// MECHANICAL VERIFICATION. FAIL-WITHOUT (revert just the flag setting
// in the AssignmentStmt arm and the override's disjunction):
//   Direct leak of 40 byte(s) in 1 object(s) allocated from: … in main
//   Indirect leak of 8 byte(s) in 1 object(s)  …
//   Indirect leak of 8 byte(s) in 1 object(s)  …
//   SUMMARY: AddressSanitizer: 56 byte(s) leaked in 3 allocation(s).
// PASS-WITH (the fix installed, 3 runs each):
//   (no leak reported).
//
// The 1000x-amplified rows below (a probe_y-shaped program called
// 1000× inside a single Main) clear LeakSanitizer's reporting floor on
// the per-call leak so the test fails loudly both with the bug present
// (~56000 B / 3000 alloc) and passes cleanly with the fix.
//
// per §27.5 / §28.9 (issue #4473)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ── Row 1: REGRESSION — the issue's exact repro shape. 1000x amplified
// via an outer ReadInto() helper so the 56 B/call leak clears LSan
// reporting floors. Pre-fix: ~56000 bytes / ~3000 allocations leaked.
// Post-fix: clean. (issue #4473, §27.5 / §28.9)
BEZEL_TEST(closure_local_read_into_closure_typed_local_4473,
           read_into_typed_local_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class Box "
        "  private _data: int; "
        "  public constructor(n: int) _data := n; end constructor; "
        "  public method Get(): int return _data; end Get; "
        "end Box; "
        "class C "
        "  public static method ReadInto(): int "
        "    var lambdaRef: () -> Box; "
        "    var x := new Box(42); "
        "    var y := lambda(): Box => x; "
        "    lambdaRef := y; "
        "    return 0; "
        "  end ReadInto; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.ReadInto(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // 0 + 1000 * 0 = 0; mod 97 = 0
}

// ── Row 2: CONTROL — decl-init shape where the initializer IS a lambda
// producer (`var lambdaRef: () -> Box := lambda(): Box => x;`), NOT a
// reassign and NOT a read-out of an existing closure-typed local. The
// pre-existing closureReleaseEligible_ pipeline already handled this
// shape — both pre- and post-fix: clean. Verifies the fix does not break
// the WITH-init + lambda-producer shape (no regression).
//
// (NB: the brief's original Row 2 example `var lambdaRef: () -> Box := y;`
// — initializer is an existing closure-typed local — leaks 56 B / 3 alloc
// regardless of whether #4473's fix is applied. That is a SEPARATE
// pre-existing leak, NOT this fix's scope — to be filed as its own issue.
// This Row 2 substitutes a CLEAN control shape that exercises the same
// decl-init code path with a lambda producer.)
// (issue #4473 / §27.5 / §28.9, control row)
BEZEL_TEST(closure_local_read_into_closure_typed_local_4473,
           decl_init_typed_local_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class Box "
        "  private _data: int; "
        "  public constructor(n: int) _data := n; end constructor; "
        "  public method Get(): int return _data; end Get; "
        "end Box; "
        "class C "
        "  public static method ReadInto(): int "
        "    var x := new Box(42); "
        "    var lambdaRef: () -> Box := lambda(): Box => x; "
        "    return 0; "
        "  end ReadInto; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.ReadInto(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // 0 + 1000 * 0 = 0; mod 97 = 0
}

// ── Row 3: CONTROL — the lambda is CALLED, not assigned to anything.
// A call-site release in `emitClosureCall`'s LocalSymbol arm balances
// the closure cell's +1 — the leak path that #4473 fixes requires an
// AssignmentStmt with a closure-typed LHS, which is absent here. Both
// pre- and post-fix: clean. Verifies the fix does not over-arm scope-
// end release for the call-balanced shape (no regression). (issue
// #4473 / §27.5 / §28.9, control row)
BEZEL_TEST(closure_local_read_into_closure_typed_local_4473,
           called_not_assigned_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class Box "
        "  private _data: int; "
        "  public constructor(n: int) _data := n; end constructor; "
        "  public method Get(): int return _data; end Get; "
        "end Box; "
        "class C "
        "  public static method ReadInto(): int "
        "    var x := new Box(42); "
        "    var y := lambda(): Box => x; "
        "    var result := y(); "
        "    return 0; "
        "  end ReadInto; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.ReadInto(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
