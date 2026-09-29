// Codegen — issue #4425 (adjacent finding, §27.5 / §28.9, v0.301):
// a closure-typed local declared WITHOUT an initializer (just a type
// annotation: `var f: () -> int;`) and then assigned a lambda via `f :=
// lambda() => 42;` was leaking the lambda's closure cell at scope end.
//
//   var f: () -> int;
//   f := lambda(): int => 42;
//   return 0;
//                  -> 40 bytes / 1 allocation leaked per call
//
// The pre-existing closureReleaseEligible_ + collectClosureReleaseEligible
// pipeline already handles the WITH-init shape (`var f := lambda() => 42;`)
// and the called shape (a reassigned + called local is balanced by the call-
// site release in `emitClosureCall`'s LocalSymbol arm). But the no-init +
// reassigned + never-called shape was missed on TWO counts:
//
//   (1) `lowerLocalDecl`'s scope-end release arm had a `v &&` guard — `v`
//       is the lowered init expression, and a `var f: () -> int;` with no
//       init lowers to nullptr. So the arm refused to arm the scope-end
//       release even when `closureReleaseEligible_` already contained the
//       decl, on the rationale "the slot is null, no value to release".
//
//   (2) `ClosureEscapeWalker`'s post-walk override (which clears
//       `escaped=true` for reassigned-and-called-or-multicalled locals so
//       they enter `closureReleaseEligible_`) required
//       `calleeCallCount > 1 || uncountableCalleeSeen` — a never-called
//       local had `calleeCallCount == 0`, so the override did not fire and
//       the local stayed marked as escaped (and so never entered
//       `closureReleaseEligible_`).
//
// Either gap alone is sufficient to leak; the lane fix closes BOTH:
//   - extends the override's disjunction to include `calleeCallCount == 0`
//     (the zero-call counter-shape — no call-site release exists to balance
//     a scope-end release, so adding the override arm is safe);
//   - adds an `isReturned_` flag to `ClosureEscapeWalker` so the override
//     does not arm scope-end release for a `return f`-transferred local
//     (the caller's release handles that slot's +1);
//   - removes the `v &&` guard from `lowerLocalDecl`'s scope-end arming so
//     the no-init typed local can arm a release whose actual dispose runs
//     at scope end (after every assignment has stored into the slot).
//
// bz_closure_release is null-safe (verified against the runtime header at
// src/runtime/bezel_runtime_refcounting.cpp line 331), so an armed release
// on a still-null slot at scope end is a no-op, and a non-null slot at
// scope end releases the cell exactly once — matching the `var f := lambda()`
// shape the existing guard accepted.
//
// MECHANICAL VERIFICATION. FAIL-WITHOUT (revert just the `v &&` removal):
//   Direct leak of 40 byte(s) in 1 object(s) allocated from:
//     … in main
//   SUMMARY: AddressSanitizer: 40 byte(s) leaked in 1 allocation(s).
// PASS-WITH (the fix installed, 3 runs each):
//   (no leak reported).
//
// The 1000x-amplified row below (probe_u × 1000 calls inside a single Main)
// clears LeakSanitizer's reporting floor on the 40 B/call leak so the
// test fails loudly both with the bug present (40000 B / 1000 alloc) and
// passes cleanly with the fix.
//
// per §27.5 / §28.9 (issue #4425)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ── Row A: the issue's exact repro shape (var f: () -> int; f := lambda()...;
// return 0;). 1000x amplified via an outer MakeClosure() so the 40 B/call leak
// clears LSan reporting floors. Pre-fix: 40000 bytes / 1000 allocations
// leaked. Post-fix: clean. (issue #4425, §27.5 / §28.9)
BEZEL_TEST(closure_local_no_init_reassign_never_called_4425,
           no_init_typed_local_reassigned_never_called_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int; "
        "    f := lambda(): int => 42; "
        "    return 0; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // 0 + 1000 * 0 = 0; mod 97 = 0
}

// ── Row B: sibling shape — no-init typed local REASSIGNED twice, never
// called. The first reassignment's displaced value is released by the
// reassign-site's own retain-before-store / release-old discipline
// (fix #4174 / #4422); the second reassignment's displaced value is
// released the same way. The FINAL value is released at scope end by the
// fix installed in this commit. Without the fix: the FINAL value leaks
// (40 B/call). 1000x amplified. (issue #4425, §27.5 / §28.9)
BEZEL_TEST(closure_local_no_init_reassign_never_called_4425,
           no_init_typed_local_two_reassignments_never_called_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int; "
        "    f := lambda(): int => 0; "
        "    f := lambda(): int => 42; "
        "    return 0; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // 0 + 1000 * 0 = 0; mod 97 = 0
}

// ── Row C: CONTROL — `var f := lambda();` (with init), not called, not
// returned. Pre-existing path already releases at scope end. Both pre- and
// post-fix: clean. Verifies the fix is sound for the WITH-init shape
// (no regression). (issue #4425 / §27.5 / §28.9, control row)
BEZEL_TEST(closure_local_no_init_reassign_never_called_4425,
           with_init_typed_local_never_called_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): int "
        "    var f: () -> int := lambda(): int => 42; "
        "    return 0; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 1000 do "
        "      total := total + C.MakeClosure(); "
        "      i := i + 1; "
        "    end; "
        "    return total mod 97; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
