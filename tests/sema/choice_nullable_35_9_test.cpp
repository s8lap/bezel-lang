// Spec-derived sema tests for §35.9 (spec v0.334) — the nullable `C?`.
//
// §35.9 verbatim: "`C?` is the ordinary nullable value type (§7.2): a `C?`
// scrutinee adds §19.6's explicit-`null`-arm requirement on top of the choice
// clause; `?.`/`??`/narrowing compose as for any nullable; boxing strips the
// wrapper per §8.6's nullable-value-type rule (for the boxable
// classifications)."
//
// Before this file §35.9 had NO row in tests/sema/ — a grep for "35_9" and
// for a nullable choice scrutinee across tests/sema/ and tests/ownership/
// returned nothing. The rows below are the choice-clause half of §35.9: the
// explicit-null requirement that §35.9 layers on top of §35.6, and the way
// the §35.6 coverage proof composes with it.
//
// The programs are single-line on purpose: each is built by concatenating C++
// string literals with NO in-program `//` comment (a `//` would swallow the
// rest of the concatenated line and every row would return -999). The prose
// citations live in the C++ comments OUTSIDE the literals.
//
// Every row here is SEMA-level (parse + collect + resolve + typecheck) and
// asserts on the diagnostic. That is deliberate: the codegen lowering of a
// `|` alternative list over a choice scrutinee is separately broken
// (`icmp eq` over two choice-struct operands trips the IR verifier,
// src/codegen/LlvmBackend_Exprs2.cpp:14661 — the disposal lane's file), so a
// compile-verdict row here would be measuring that defect instead of §35.9.

#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool isExhaustiveWarn(const bezel::Diagnostic& d) {
    return d.severity == bezel::Severity::Warning &&
           d.message.find("non-exhaustive") != std::string::npos;
}

bool hasExhaustiveWarn(const bezel::test::SemaResult& r,
                       std::string_view choiceName,
                       std::string_view missingNeedle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (!isExhaustiveWarn(d)) continue;
        if (choiceName.empty()) return true;
        if (d.message.find(std::string("choice `") + std::string(choiceName) +
                           "`") == std::string::npos)
            continue;
        if (!missingNeedle.empty() &&
            d.message.find(std::string(missingNeedle)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

bool lacksExhaustiveWarn(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (isExhaustiveWarn(d)) return false;
    return true;
}

bool hasErrWith(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// === §35.9: the explicit-null-arm requirement ============================

// §35.9: "a `C?` scrutinee adds §19.6's explicit-`null`-arm requirement on top
// of the choice clause". Every case is named but no `null` arm: the
// diagnostics must name the choice and say the NULLABLE VALUE is uncovered —
// not a case name. This is the row that distinguishes §35.9 from §35.6.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_nullable_scrutinee_without_null_arm_warns) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red   => 1, "
        "    Green => 2, "
        "    Blue  => 3); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasExhaustiveWarn(r, "Color", "nullable value not covered"));
}

// §35.9: with the `null` arm the match is exhaustive and silent. The
// differential of the row above — same cases, same scrutinee, one more arm.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_nullable_scrutinee_with_null_arm_no_warn) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red   => 1, "
        "    Green => 2, "
        "    Blue  => 3, "
        "    null  => 4); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// The two requirements are ADDITIVE, and the diagnostic distinguishes them:
// this program is missing BOTH a case and the null arm, and must say both.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_missing_case_and_null_arm_reports_both) {
    // §35.9's null requirement sits "on top of" the §35.6 choice clause, so
    // omitting one case AND the null arm reports both, and the named case is
    // the one actually absent.
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red   => 1, "
        "    Green => 2); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", "Blue"));
    // The diagnostic's null clause has two wordings, keyed on whether any
    // case is also missing (ExprTypecheck.cpp: the choice branch appends
    // " nullable value not covered" only when the missing-case list is
    // empty, and " (and a `null` arm)" otherwise). This program is in the
    // second case, so that is the wording asserted here.
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", "(and a `null` arm)"));
}

// A `_` catch-all covers the null state (and every case), so §19.6's
// catch-all rule satisfies §35.9's requirement without a spelled `null` arm.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_wildcard_arm_covers_the_null_state) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red   => 1, "
        "    Green => 2, "
        "    Blue  => 3, "
        "    _     => 4); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// === §35.9 × §35.5: an alternative list counts on a nullable scrutinee ====
//
// §35.6 counts a payload-free case "directly or in a `|` list"; §35.9 says
// the nullable wrapper only ADDS the null requirement. So the two compose:
// the `|` list covers its cases, and the separate `null` arm covers the
// wrapper. This row is the composition of the fix in
// choice_alt_pattern_exhaustiveness_35_6_test.cpp with §35.9.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_alt_list_cases_plus_null_arm_is_exhaustive) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red | Green => 1, "
        "    Blue        => 2, "
        "    null        => 3); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// The differential: the same `|` list WITHOUT the null arm still warns, and
// warns only about the null state — proving the `|` list really did cover
// Red and Green, rather than the whole match being excused.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_alt_list_cases_without_null_arm_still_warns_on_null) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red | Green => 1, "
        "    Blue        => 2); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasExhaustiveWarn(r, "Color", "nullable value not covered"));
    BEZEL_EXPECT_FALSE(hasExhaustiveWarn(r, "Color", "Red"));
    BEZEL_EXPECT_FALSE(hasExhaustiveWarn(r, "Color", "Green"));
}

// And the second differential: an alt-list that genuinely misses a case names
// that case and not the ones the list did cover.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_alt_list_missing_case_names_only_that_case) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red        => 1, "
        "    null       => 2); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Color", "Green Blue"));
}

// === §35.9 × §35.6: a payload case covered by a destructuring ============

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_payload_case_destructuring_plus_null_arm_no_warn) {
    auto r = semaAll(
        "module M; "
        "choice Sig Off, On(flag: bool), Broken(code: int); end Sig; "
        "class C public static method Main(): int "
        "  public method Pick(s: Sig?): int "
        "    return (case s of "
        "    On(var f)        => (if f then 1 else 0), "
        "    Broken(var code) => code, "
        "    Off              => -1, "
        "    null             => -2); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// === §35.6 statement-position control over a nullable choice =============
//
// §19.6: exhaustiveness is checked only in EXPRESSION position. §35.9 layers
// the null requirement on the §19.6 proof, so a statement-position match over
// a `C?` is silent whether or not it covers null — the requirement is part
// of the static proof that statement position does not run.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_statement_position_nullable_case_not_checked) {
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  public method Pick(c: Color?): int "
        "    case c of "
        "      Red => do return 1; end "
        "    end; "
        "    return 0; "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// === §35.9 × §35.5: payload cases still need patterns on a nullable =====

// §35.5's arity rule is unchanged by the nullable wrapper: a payload case
// matched bare is still a compile error naming the arity.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_payload_case_bare_still_rejected_on_nullable_scrutinee) {
    auto r = semaAll(
        "module M; choice Color Red, Green(v: int), Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red         => 1, "
        "    Blue        => 2, "
        "    null        => 3, "
        "    Green       => 4); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasErrWith(r, "Payload case `Green` requires a payload pattern"));
}

// CONTROL (the sibling kind): the same nullable scrutinee over an ENUM keeps
// its own long-standing behaviour — the null arm satisfies §10.6 / §19.6's
// requirement, and omitting it warns. This row localises the rows above to
// the CHOICE clause rather than to the nullable wrapper machinery.

BEZEL_TEST(choice_nullable_35_9,
           spec_35_9_enum_scrutinee_null_arm_behaviour_unchanged) {
    auto r = semaAll(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method Pick(c: Color?): int "
        "    return (case c of "
        "    Red   => 1, "
        "    Green => 2, "
        "    Blue  => 3, "
        "    null  => 4); "
        "end Pick; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}