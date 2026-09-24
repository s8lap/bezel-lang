// Regression tests for issue #4809 — residual of narrow #4766 (lane r20-l3,
// commit 6173e0c6): the §5.1 implicit-conversion check was skipped for
// unannotated `DateTime.UtcNow` / `DateTime.Now` / `DateTime.Today` /
// `DateTime.MinValue` / `DateTime.MaxValue` / `TimeSpan.Zero` reads, because
// the existing static-property typing at L19712 (ExprTypecheck_Calls.cpp) is
// gated on `!targetType` and the static-member branch sets `targetType` to
// the DateTime/TS-builtin TypeSymbol BEFORE reaching it. With the source
// UNTYPED, `checkAssignable` saw a null source and §5.1 silently passed —
// `var n: int := DateTime.UtcNow;` accepted, no diagnostic. Same defect class
// as #4760 / #4766 (the wrong-value class — `n` would receive the raw
// DateTime i32 bit-pattern reinterpreted as an int).
//
// Spec §5.1 (v0.307) lists no row from DateTime or TimeSpan to a primitive;
// §4.2 names the integer types but never references DateTime/TimeSpan as a
// source. So `var n: int := DateTime.UtcNow` / `var n: int := TimeSpan.Zero`
// MUST be REJECT.
//
// Cite: spec §5.1, §4.2, v0.307; stdlib §2.5 (DateTime.UtcNow: DateTime,
// TimeSpan.Zero: TimeSpan).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;

namespace {

// Search for the §5.1-style implicit-conversion failure in the diagnostic
// stream. The fix sets exprType to a Named DateTime / TimeSpan TypeRef with
// declaration = builtinClassSym(...) (matches §2.5's `DateTime.UtcNow:
// DateTime` / `TimeSpan.Zero: TimeSpan` member-typing shape), so the
// `!from->declaration` arm of the L5239 rule (which fires for the
// declaration-LESS, declaration-less-Named shape from `var d := …`) does
// NOT apply; checkAssignable reaches the GENERIC catch-all at L4345, which
// emits "the initializer for `n` has a type that is not implicitly
// convertible to the target type (§5.1)". The §5.1 reference is the
// load-bearing part — it is the rule that is supposed to reject, and the
// suffix is one string in src/.
constexpr const char* kRuleWording =
    "not implicitly convertible to the target type (§5.1)";

bool rejectedByRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWording) != std::string::npos) return true;
    }
    return false;
}

bool accepted(const std::string& program) {
    auto r = runAot(program);
    return r.ran;
}

std::string wrap(const std::string& body) {
    return
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    " + body + "\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

// ═════════ REJECT rows — the §5.1 over-acceptance pinned as the bug ═════════

// spec §5.1 / §4.2, v0.307: no implicit conversion DateTime → int.
// Pre-fix: accepted silently (the source was untyped, checkAssignable was
// skipped, and codegen read an i32 slot as if it were a DateTime — same
// defect class as #4760/#4766).
BEZEL_TEST(static_member_assignability_4809,
           static_member_datetime_utcnow_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        wrap("var n: int := DateTime.UtcNow;")));
}

BEZEL_TEST(static_member_assignability_4809,
           static_member_datetime_now_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        wrap("var n: int := DateTime.Now;")));
}

BEZEL_TEST(static_member_assignability_4809,
           static_member_timespan_zero_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        wrap("var n: int := TimeSpan.Zero;")));
}

// ═════════ Accept controls — same source, valid target type ═══════════════

// Regression guard: same source (DateTime.UtcNow), DateTime target — must
// keep ACCEPTING (the source's Named DateTime typeref matches the target's
// by identity per §5.1 "every type converts to itself").
BEZEL_TEST(static_member_assignability_4809,
           static_member_control_negative_datetime_to_datetime) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap("var d: DateTime := DateTime.UtcNow;")));
}

// Regression guard: same source (TimeSpan.Zero), TimeSpan target.
BEZEL_TEST(static_member_assignability_4809,
           static_member_control_negative_timespan_to_timespan) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap("var t: TimeSpan := TimeSpan.Zero;")));
}

}  // namespace