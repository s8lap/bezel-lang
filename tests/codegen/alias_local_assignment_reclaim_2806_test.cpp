// Codegen / refcounting — issue #2806: assigning a heap-produced `string` local
// into an ALREADY-DECLARED local (`alias := built;`) leaked the source. This
// is the AssignmentStmt twin of #2764, which closed the LocalDecl form (`var
// other := built;`): that fix relaxed the walker only for the declaration
// node, so the assignment arm fell through to the conservative escape walk and
// continued to treat `built`'s appearence as an ESCAPE. One heap string (the
// `bz_string` header plus its data buffer, two allocations under LSan) per
// assignment.
//
// Spec: §4.6 "`string`"; §28.9 "Runtime-managed types" — "`string` — pervasive;
// created by literals, interpolation, concatenation, and library methods"; §28.3
// "Local variables, ownership, and borrows", kernel K1.6 — "Values of
// runtime-managed type (`string`, boxed value types, function values) and of
// ordinary value type carry no classification and no obligation". Spec revision
// v0.282.
//
// So neither `built` nor `alias` carries a §28 disposal obligation; what each
// carries in THIS implementation is one reference, and the observable
// requirement is that the heap string be reclaimed. It was not.
//
// THE DEFECT. The producer-side scope-end release is gated on the
// StringEscapeWalker proving the local never escapes, and the AssignmentStmt
// arm's fallthrough after the `+=` (#1486), self-reassign (#1997),
// class/interface property-SET (#2505/#2535) and indexer-SET (#2540)
// relaxations walked the RHS as a non-safe read. So a bare-identifier RHS
// naming the tracked local was an ESCAPE — `built` lost its scope-end
// release, the producer's +1 leaked. Every `alias := built;` in a loop or
// long-running service leaks one string, compounding per iteration.
//
// WHY IT IS SAFE TO RELAX. #2764 already gives EVERY borrow-initialized
// string local an INDEPENDENT +1: armStringLocalRefcount emits
// `bz_string_retain` into the new slot for every member of `stringBorrowAll_`,
// escaping or not. So when `alias := built;` runs, `alias`'s slot ALREADY
// mints its own +1 into the heap string — the retain is the assignment-site
// equivalent of the `var other := built;` declaration retain. The scope-end
// release of `built`'s slot then drops only `built`'s own +1, leaving
// `alias`'s independent +1 free to be dropped by `alias`'s own scope-end
// release. The discipline is the SAME coordinated shape as #2764's
// declaration arm: the alias keeps its own +1, the source keeps its own
// release. The predicate on the relaxation — the assignment TARGET must be
// a member of `stringBorrowAll_` (the exact set armStringLocalRefcount tests
// before emitting `bz_string_retain` at the assignment site) — guarantees
// exactly that paired retain, so the two halves cannot disagree.
//
// Relaxing without that predicate (e.g. for a `string?` slot, a `using var`,
// or a target outside stringBorrowAll_) would free `built` while an
// un-retained alias still named it — a use-after-free, strictly worse than
// this bounded leak. So the gate is membership in `stringBorrowAll_` itself,
// passed into the walker by pointer (the same `borrowRetainedAliases` field
// #2764 introduced), and the relaxation fires only when target is an
// identifier naming such a local AND the RHS is the tracked local AND
// `stringReleaseMode` is set (matching every other relaxation on the walker).
//
// ⚠ Every string here is built at RUNTIME (`$"ov-{i}"`): `"ab" + "cd"`
// constant-folds to an rc sentinel on which `bz_string_release` no-ops — the
// trap that has made probes in this family vacuous repeatedly.
//
// ⚠ EVERY case is LOOP-AMPLIFIED (and each iteration allocates a churn
// string). This is load-bearing, not decoration: a ONE-assignment `Main` is
// NOT a reliable LSan oracle. A leftover pointer in a register or stack
// slot makes LSan treat the block as reachable. Twenty iterations put the
// leak beyond that noise.
//
// MECHANICAL FAIL -> PASS (--sanitize=address, per case):
//   (A) 751 B / 40 allocs  -> clean      (the issue's own repro, loop amplified)
//   (B) 751 B / 40 allocs  -> clean      (assignment to a borrowed string slot)
//   (C) clean both before and after      (self-assign `a := a` — covered by #1997,
//                                        pinned here so the new relaxation does
//                                        not regress that path)
// The PLAIN (unsanitized) exit code is IDENTICAL before and after in every
// case — a leak is invisible without a sanitizer, so LSan is the oracle that
// moves and the plain assertions pin that nothing is freed early.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// sum over i in 1..20 of len("ov-{i}") = 9*4 + 11*5 = 91.
constexpr int kLenSum20 = 91;

// The churn allocation every case makes between the assignment and the reads.
// The `< 0` test is never true; it exists to force the read so the local cannot
// be elided, and the allocate/free pair means a prematurely freed block cannot
// be read back intact on the plain (unsanitized) path.
#define CHURN                                                                  \
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                  \
    "      if churn.Length < 0 then return 99; end; "

// =====================================================================
// (A) THE ISSUE'S OWN REPRO (loop-amplified): `alias := built;` where
// `alias` is an already-declared string local and `built` is a fresh
// interpolation producer in a narrower scope. §4.6 / §28.9.
// =====================================================================
const char* kAssignIntoAlreadyDeclaredLocal =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var alias := \"\"; "
    "    for var i in 1..20 do "
    "      if i > 0 then "
    "        var built := $\"ov-{i}\"; "
    "        alias := built; "
    "      end; "
    CHURN
    "      total := total + alias.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (B) THE ASSIGNMENT FORM WITHOUT THE NARROWER SCOPE: `alias := built;`
// sits inside the SAME loop, both locals in the loop body. Identical
// defect shape to (A) — `built` is a +1 producer, `alias` is a
// stringBorrowAll_ member, the walker's AssignmentStmt arm must reach the
// source-as-safe-read relaxation. §28.3 K1.6 / §28.9.
// =====================================================================
const char* kAssignInsideLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var alias := \"\"; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var built := $\"oa-{i}\"; "
    "      alias := built; "
    CHURN
    "      total := total + alias.Length; "
    "    end; "
    "    return total; "                    // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (C) CONTROL: `alias := alias;` — the self-reassign of #1997. Pin that
// the new relaxation does NOT regress the existing reassignment-eligible
// path. The predicate that fires for #1997 (target IS the tracked local)
// is disjoint from the predicate for #2806 (value IS the tracked local)
// and both must fire correctly under `stringReleaseMode`. Clean both
// before and after.
// =====================================================================
const char* kSelfReassignControl =
    "module M; class C "
    "  public static method Main(): int "
    "    var alias := \"\"; "
    "    for var i in 1..20 do "
    "      alias := $\"sa-{i}\"; "
    CHURN
    "      if alias.Length < 0 then return 99; end; "
    "    end; "
    "    return alias.Length; "             // 5
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           assign_into_already_declared_local_is_released_spec_28_9) {
    JitResult r = runAotChecked(kAssignIntoAlreadyDeclaredLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           assign_into_already_declared_local_plain_value_invariant_spec_28_9) {
    // NOT a #2806 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that the fix did not change the
    // computed value, i.e. that nothing was freed early.
    JitResult r = runAot(kAssignIntoAlreadyDeclaredLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (B) identical-shape loop variant without the narrower scope.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           assign_inside_loop_is_released_spec_28_3) {
    JitResult r = runAotChecked(kAssignInsideLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);              // 751 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           assign_inside_loop_plain_value_invariant_spec_28_3) {
    JitResult r = runAot(kAssignInsideLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (C) self-reassign control. Clean both before and after — pins that the
// relaxation does not regress #1997's path.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           self_reassign_control_clean_spec_28_3) {
    JitResult r = runAotChecked(kSelfReassignControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

BEZEL_TEST(codegen_alias_local_assignment_reclaim_2806,
           self_reassign_control_plain_value_invariant_spec_28_3) {
    JitResult r = runAot(kSelfReassignControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
