// Regression tests for issue #4917 — stdlib §2.5 (v0.307) says DateTime and
// TimeSpan declare NO relational operators (`<`, `>`, `<=`, `>=`); only
// `CompareTo` (an `IComparable<T>` member, returns int) and `=`/`<>` equality
// (auto-gen per §9.2). The compiler still ACCEPTED `t > 621355968000000000L`
// (DateTime > long) and `(t2 - t1) >= 0L` (TimeSpan >= long) because
// DateTime/TimeSpan lower to a scalar i64 tick count and the record/class/
// choice relational-operator guard excluded the value-builtins (thinking the
// scalar representation implied relational was defined). #4809 (f027404d)
// typed the arithmetic operators as intrinsics but did not gate the
// relational ones.
//
// Pre-fix: `t > 621355968000000000L` accepted, codegen emitted a tick-count
// `icmp`, program ran. After the fix, both forms reject at sema with a
// diagnostic that cites §2.5 and points the user at `CompareTo` or `.Ticks`.
// Equality (`=`, `<>`) and the §2.5-intrinsic arithmetic (`+`, `-` between
// the two types and a TimeSpan) are unaffected.
//
// §18 blast radius (measured pre-fix at r20-c25, 216b1cb8):
//   - 2 rows in tests/codegen/datetime_timespan_test.cpp (`utcnow_ticks_positive`,
//     `utcnow_monotone`) pinned the bug; both migrated to `.Ticks` form
//     in this commit.
//   - All other DateTime/TimeSpan references in tests/, tests/lit/, and
//     tests/conformance/ already use `.Ticks` / `.TotalSeconds` /
//     `CompareTo(...)` / `.ToString()` (no relational on the bare value).
//   - tests/codegen/datetime_operator_intrinsics_4809_test.cpp uses
//     `d.Ticks >= 0L` and `d.TotalSeconds < 60.0` (LHS is `.Ticks`/`.TotalSeconds`,
//     both primitives, never the bare DateTime/TimeSpan) — unaffected.
//
// Cite: stdlib §2.5 (line ~262), §25, v0.307.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;

namespace {

// The fix's diagnostic wording — one occurrence in src/ (the new block in
// ExprTypecheck_Exprs.cpp typeBinary). Match on the unique spec citation so
// the row fails loudly if a future edit moves the wording or replaces the
// reject with a different rule.
constexpr const char* kRuleWording =
    "operator `>` is not defined for type `DateTime` (stdlib §2.5, v0.307;"
    " use CompareTo or .Ticks)";

// `t < …`, `t <= …` (different operator) — same rule wording shape, but the
// fix embeds the operator in the message. Match on the substring that is
// stable across the operator glyph (`is not defined for type`) AND the
// spec citation (`stdlib §2.5, v0.307`).
constexpr const char* kRuleWordingSubstr =
    "is not defined for type";

bool rejectedBy4917(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWordingSubstr) == std::string::npos) continue;
        if (d.message.find("stdlib §2.5, v0.307") == std::string::npos) continue;
        return true;
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
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

// ═════════ REJECT rows — the bug pinned ═══════════════════════════════════

// spec §2.5, v0.307: DateTime declares no `>` overload. Pre-fix: the
// record/class/choice relational guard excluded value-builtins (DateTime
// carries a scalar i64 tick count, so the compiler silently emitted a
// tick-count `icmp`). Post-fix: reject with the rule wording.
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           datetime_gt_long_rejected_spec_2_5) {
    BEZEL_EXPECT_TRUE(rejectedBy4917(
        wrap(
            "var t: DateTime := DateTime.UtcNow;\n"
            "if t > 621355968000000000L then return 1; end;\n"
        )));
}

// spec §2.5, v0.307: TimeSpan declares no `>=` overload. The expression
// `(t2 - t1)` is a TimeSpan (per #4809), so this is the second pinned
// repro from the issue body — must reject too. Pre-fix: accepted.
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           timespan_ge_long_rejected_spec_2_5) {
    BEZEL_EXPECT_TRUE(rejectedBy4917(
        wrap(
            "var t1: DateTime := DateTime.UtcNow;\n"
            "var t2: DateTime := DateTime.UtcNow;\n"
            "if (t2 - t1) >= 0L then return 1; end;\n"
        )));
}

// ═════════ ACCEPT controls — same operands, idiomatic spellings ═══════════

// `.Ticks` returns long; the LHS of `>` is `long`, not DateTime. Spec §2.5
// names `.Ticks` as the canonical way to inspect a DateTime's tick count.
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           datetime_ticks_gt_long_accepted_control) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap(
            "var t: DateTime := DateTime.UtcNow;\n"
            "if t.Ticks > 621355968000000000L then return 1; end;\n"
        )));
}

// `CompareTo(other)` returns `int`; the LHS of `<` is `int`. Spec §2.5
// names `CompareTo` as the ordering-only member (returns negative/0/positive).
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           datetime_compareto_lt_int_accepted_control) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap(
            "var t: DateTime := DateTime.UtcNow;\n"
            "var other: DateTime := DateTime.UtcNow;\n"
            "if t.CompareTo(other) < 0 then return 1; end;\n"
        )));
}

// `(t2 - t1)` is a TimeSpan (§2.5 / #4809); `.Ticks` returns long.
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           timespan_ticks_ge_long_accepted_control) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap(
            "var t1: DateTime := DateTime.UtcNow;\n"
            "var t2: DateTime := DateTime.UtcNow;\n"
            "if (t2 - t1).Ticks >= 0L then return 1; end;\n"
        )));
}

// Equality is unaffected — records auto-generate `=` and `<>` per §9.2
// regardless of whether ordering is defined. Regression guard against the
// fix accidentally widening to equality.
BEZEL_TEST(datetime_timespan_relational_reject_4917,
           datetime_equality_unchanged_control) {
    BEZEL_EXPECT_TRUE(accepted(
        wrap(
            "var t1: DateTime := DateTime.UtcNow;\n"
            "var t2: DateTime := DateTime.UtcNow;\n"
            "if t1 = t2 then return 0; end;\n"
            "if t1 <> t2 then return 0; end;\n"
            "return 1;\n"
        )));
}

}  // namespace
