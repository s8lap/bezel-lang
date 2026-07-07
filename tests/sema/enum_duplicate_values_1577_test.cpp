// Spec-derived tests for §10.2 (duplicate enum constant values, v0.182,
// issue #1577).
//
// §10.2: "Two or more constants of one enum may share an underlying value,
// whether written explicitly (`Legacy := 1, Current := 1`) or produced by
// auto-increment continuation. Aliasing is legal and draws no diagnostic;
// but since an enum value *is* its underlying integer, same-valued
// constants denote ONE value... in a `case` over the enum, two arms — or
// two `|` alternatives — naming same-valued constants are a COMPILE ERROR,
// the later one being unreachable; exhaustiveness (§19.6) is over the set
// of distinct declared *values*, so an arm naming either alias covers
// both."
//
// These tests cover: (1) the enum DECLARATION itself never diagnoses
// aliasing (legal, per §10.2's first sentence); (2) a `case` naming two
// same-valued constants in different arms is a compile ERROR, whether the
// collision is explicit or produced by auto-increment continuation; (3) a
// `case` naming two same-valued constants in one `|` alternative list is
// also a compile error; (4) distinct-valued constants are unaffected; (5)
// an arm naming one alias satisfies exhaustiveness for its sibling alias
// too (§19.6 "an arm naming either alias covers both").

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic at the given severity contains `needle`.
bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              bezel::Severity sev, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == sev && d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

bool hasNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    return hasDiagnosticContaining(r, bezel::Severity::Warning,
                                    "non-exhaustive");
}

} // namespace

// --- Declaration-level aliasing stays legal (§10.2 first sentence) --------

// `Legacy := 1, Current := 1` at the declaration itself draws no diagnostic;
// only a later `case` naming both is restricted (tested below).
BEZEL_TEST(sema_enum_dup_values,
           declaration_level_aliasing_is_legal_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A := 1, B := 1; end E; "
        "class C "
        "  public method M(): int "
        "    return int(E.A); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- `case` arms naming an EXPLICIT duplicate value (§10.2) ---------------

// `A := 1, B := 1`; a `case` with an unguarded arm for `A` followed by an
// arm for `B` is a compile error — `B` denotes the same value as `A` and can
// never be reached.
BEZEL_TEST(sema_enum_dup_values,
           case_arm_explicit_duplicate_value_is_error_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A := 1, B := 1; end E; "
        "class C "
        "  public method M(v: E): int "
        "    return case v of "
        "      A => 1, "
        "      B => 2 "
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, bezel::Severity::Error, "\xc2\xa7""10.2"));
}

// --- `case` arms naming a value collision produced by AUTO-INCREMENT ------

// `A := 1, B, C := 2`; `B` auto-increments to 2 and collides with the
// explicit `C := 2`. A `case` naming `B` then `C` is a compile error exactly
// as the explicit-duplicate case above (§10.2: "whether written explicitly
// ... or produced by auto-increment continuation").
BEZEL_TEST(sema_enum_dup_values,
           case_arm_auto_increment_collision_is_error_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A := 1, B, C := 2; end E; "
        "class D "
        "  public method M(v: E): int "
        "    return case v of "
        "      B => 1, "
        "      C => 2, "
        "      _ => 0 "
        "    end; "
        "  end M; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, bezel::Severity::Error, "\xc2\xa7""10.2"));
}

// --- Two `|` alternatives naming the same value in ONE arm (§10.2) --------

BEZEL_TEST(sema_enum_dup_values,
           alternative_list_duplicate_value_is_error_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A := 1, B := 1, C := 2; end E; "
        "class D "
        "  public method M(v: E): int "
        "    return case v of "
        "      A | B => 1, "
        "      C => 2 "
        "    end; "
        "  end M; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, bezel::Severity::Error, "\xc2\xa7""10.2"));
}

// --- Distinct values are unaffected (positive control) --------------------

// All three constants hold DISTINCT explicit values; the `case` names each
// exactly once and must compile with no diagnostic at all.
BEZEL_TEST(sema_enum_dup_values, case_arms_distinct_values_ok_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum E A := 1, B := 2, C := 3; end E; "
        "class D "
        "  public method M(v: E): int "
        "    return case v of "
        "      A => 1, "
        "      B => 2, "
        "      C => 3 "
        "    end; "
        "  end M; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
}

// A `|` alternative list over DISTINCT values (§10.6-style grouping) is
// unaffected by the duplicate-value check.
BEZEL_TEST(sema_enum_dup_values,
           alternative_list_distinct_values_ok_spec_10_2) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    return case c of "
        "      Red | Green => 1, "
        "      Blue => 2 "
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
}

// --- Exhaustiveness is by distinct VALUE, not by declared name (§19.6) ----

// `Legacy` and `Current` share value 1; an arm naming only `Legacy` (plus one
// covering the distinct `Other := 2`) is exhaustive — "an arm naming either
// alias covers both" (§10.2 / §19.6). No non-exhaustiveness warning.
BEZEL_TEST(sema_enum_dup_values,
           exhaustiveness_arm_naming_one_alias_covers_both_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum E Legacy := 1, Current := 1, Other := 2; end E; "
        "class D "
        "  public method M(v: E): int "
        "    return case v of "
        "      Legacy => 1, "
        "      Other => 2 "
        "    end; "
        "  end M; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
}

// A genuinely missing (uncovered) constant still warns — the value-based
// exhaustiveness rule only merges ALIASES of a covered value; it does not
// suppress the warning for an unrelated, truly-uncovered value.
BEZEL_TEST(sema_enum_dup_values,
           exhaustiveness_still_warns_on_genuinely_missing_value_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    return case c of "
        "      Red => 1, "
        "      Green => 2 "
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
