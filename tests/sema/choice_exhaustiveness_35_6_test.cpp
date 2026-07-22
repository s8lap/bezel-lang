// Spec-derived sema tests for §35.6 (v0.200, #2045): choice exhaustiveness.
//
// §35.6 "Exhaustiveness": "The static-proof rule for choice scrutinees is
// §19.6's choice bullet: coverage is counted per case name, one level deep
// — a payload-free case by a constant-style pattern naming it (directly or
// in a `|` list), a payload case by a destructuring whose every slot is
// irrefutable. Guarded arms and refutable-slot arms contribute nothing to
// the static proof."
//
// §19.6 (carried over): a non-exhaustive expression-position `case` draws
// a WARNING ("the compiler warns when it cannot prove exhaustiveness
// statically"). The diagnostic names the missing case(s). A statement-
// position `case` is not exhaustiveness-checked (unmatched values fall
// through silently per §19.6).

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

// True iff a non-exhaustiveness WARNING fired naming the choice + the
// `missingNeedle` case.
bool hasExhaustiveWarn(const bezel::test::SemaResult& r,
                       std::string_view choiceName,
                       std::string_view missingNeedle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("non-exhaustive") == std::string::npos) continue;
        if (d.message.find(std::string("choice `") +
                           std::string(choiceName) + "`") == std::string::npos)
            continue;
        if (!missingNeedle.empty() &&
            d.message.find(std::string("missing case(s):") +
                           std::string(missingNeedle)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// True iff NO non-exhaustiveness warning fired at all (a fully-covered
// expression-position `case` over a choice should not warn).
bool lacksExhaustiveWarn(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("non-exhaustive") != std::string::npos) return false;
    }
    return true;
}

}  // namespace

// --- §35.6: full coverage does not warn (payload-free + payload cases) -----

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_full_coverage_payload_free_choice_no_warn) {
    // §35.6: every declared case covered ⇒ exhaustive ⇒ no warning.
    auto r = semaAll(
        "module M; choice Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x: Color := Color.Red; "
        "  return (case x of "
        "    Red   => 1, "
        "    Green => 2, "
        "    Blue  => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_full_coverage_payload_choice_no_warn) {
    // §35.6: a payload case is covered by a destructuring with irrefutable
    // slots (here: `var v`). Both cases covered ⇒ no warning.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(var v) => v, "
        "    None        => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_full_coverage_with_discard_slot_no_warn) {
    // §35.6: `_` is irrefutable, so `Some(_)` covers the Some case.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(_) => 1, "
        "    None    => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_full_coverage_naked_binding_slot_no_warn) {
    // §35.6: a naked identifier slot is irrefutable.
    auto r = semaAll(
        "module M; choice Pair First(a: int, b: int), Zero; end Pair; "
        "class C public static method Main(): int "
        "  var x: Pair := Pair.Zero; "
        "  return (case x of "
        "    First(a, b) => a + b, "
        "    Zero         => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_catch_all_arm_no_warn) {
    // §19.6 (carries over): an unguarded `_` arm is a catch-all and is
    // always exhaustive, even over a choice scrutinee.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(var v) => v, "
        "    _           => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}

// --- §35.6: partial coverage warns naming the missing case ---------------

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_missing_payload_free_case_warns) {
    // §35.6: missing the payload-free case `None` ⇒ warning naming it.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(var v) => v); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Opt", " None"));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_missing_payload_case_warns) {
    // §35.6: missing the payload case `Some` ⇒ warning naming it.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    None => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Opt", " Some"));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_missing_one_of_three_cases_warns) {
    // §35.6: with three cases and two covered, the third is named.
    auto r = semaAll(
        "module M; choice Tri A, B, C; end Tri; "
        "class D public static method Main(): int "
        "  var x: Tri := Tri.A; "
        "  return (case x of "
        "    A => 1, "
        "    B => 2); "
        "end Main; end D;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Tri", " C"));
}

// --- §35.6: guarded arms + refutable slots do not contribute ---------------

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_guarded_arm_does_not_contribute) {
    // §35.6: "Guarded arms ... contribute nothing to the static proof."
    // The single guarded `Some(var v) when ...` arm does not cover Some.
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  return (case x of "
        "    Some(var v) when v > 0 => v, "
        "    None                   => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasExhaustiveWarn(r, "Opt", " Some"));
}

BEZEL_TEST(choice_exhaustiveness_35_6,
           spec_35_6_statement_position_no_check) {
    // §19.6: a non-exhaustive STATEMENT-position `case` does NOT warn —
    // unmatched values fall through silently. (Exhaustiveness is checked
    // only in expression position where MatchFailureException applies.)
    auto r = semaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C public static method Main(): int "
        "  var x: Opt := Opt.None; "
        "  case x of "
        "    Some(var v) => do return v; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(lacksExhaustiveWarn(r));
}
