// fix #5474 (sema, §19.3, v0.315):
// `bezelc` segfaulted when a `case` had an unresolved scrutinee AND at
// least one range/relational pattern. The crash was in
// `getTypeNameForOrderingDiag(scrutTy)` at ExprTypecheck.cpp:13833:
// `ty->kind` dereferenced `ty` without checking for null when `scrutTy`
// was null (unresolved scrutinee → no type set). The fix adds a `!ty`
// null guard to `getTypeNameForOrderingDiag`, so the function returns "?"
// instead of crashing. The established diagnostic for this situation is
// "unresolved identifier `name`".
//
// Spec §19.3 (v0.315): a relational pattern "compares the scrutinee
// against the other-hand operand, under the intrinsic ordering of the
// scrutinee's type". With an unresolved scrutinee there is no type, so
// the rule's precondition is unmet and it must not be evaluated.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — unresolved scrutinee with range pattern: must not crash,
// must report "unresolved identifier". The fix guards getTypeNameForOrderingDiag
// against a null TypeRef*; without the fix bezelc dies with SIGSEGV.
BEZEL_TEST(sema_case_range_over_unresolved_scrutinee_5474,
           unresolved_scrutinee_with_range_pattern_clean_reject) {
    auto r = runAot(
        "module Conf; "
        "class W "
        "  public static method Main() "
        "    var grade := (case score of "
        "      90..100 => \"A\", "
        "      80..89 => \"B\", "
        "      70..<80 => \"C\", "
        "      _ => \"F\"); "
        "  end Main; "
        "end W;");
    // Must be a compile-time rejection with a real source location, NOT a crash.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The scrutinee `score` must be reported as unresolved.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "unresolved identifier"));
}
