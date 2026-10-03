// Spec-derived regression tests for fix #5235 — §19.3 (v0.313).
//
// A relational or range pattern over an Object scrutinee lowers to malformed IR
// (`icmp sgt ptr %obj, i32 5`) because Object has no defined ordering. The
// spec says relational patterns compare under the scrutinee's type ordering
// (§19.3), and Object has no ordering. The compiler must reject at sema rather
// than emitting malformed IR.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool hasObjectOrderingDiag(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Object has no defined ordering") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// §19.3 (v0.313, #5235): RelOpPattern over Object — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_gt_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of > 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (>=) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_gte_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of >= 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (<) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_lt_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of < 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RelOpPattern over Object (<=) — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           relop_lte_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of <= 5 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// §19.3 (v0.313, #5235): RangePattern over Object — REJECT
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           range_over_object_rejected) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of 1..10 => 1, _ => 2); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasObjectOrderingDiag(r));
}

// Control: a discard pattern over Object compiles cleanly — the fix fires
// only on RelOpPattern and RangePattern, not on other pattern kinds.
BEZEL_TEST(object_scrutinee_relop_pattern_rejected_5235,
           discard_pattern_over_object_compiles) {
    auto r = runAot(
        "module M; class P "
        "  public static method Main(): int "
        "    var o: Object := null; "
        "    return (case o of _ => 1); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
