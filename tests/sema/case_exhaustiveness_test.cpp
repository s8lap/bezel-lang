// Spec-derived tests for §19.6 (case exhaustiveness).
//
// §19.6 is precise on TWO points that shape these tests:
//
//   1. SEVERITY. "A `case` with no `_` arm is permitted. In statement
//      position, an unmatched value falls through with no action. In
//      expression position, an unmatched value raises MatchFailureException
//      at runtime; the compiler WARNS when it cannot prove exhaustiveness
//      statically." Non-exhaustiveness is therefore a WARNING (not an
//      error) and fires only in EXPRESSION position. A statement-position
//      `case` is exempt — it falls through silently. These tests assert on
//      the presence/absence of a `Severity::Warning` diagnostic, and that
//      no diagnostic is ever promoted to an error (compilation still
//      succeeds).
//
//   2. PROVABLE SHAPES. The compiler proves exhaustiveness for: an enum
//      scrutinee (every declared constant matched), a bool scrutinee (both
//      `true` and `false`), a sealed-abstract closed class hierarchy (every
//      leaf matched, §11.7), and the nullable variants of those (the above
//      plus an explicit `null` pattern). An unguarded `_` or unconstrained
//      `var x` arm is a catch-all that is always exhaustive. Every other
//      shape (open class hierarchy, int/string/range scrutinee) cannot be
//      proven exhaustive, so the warning fires unless a catch-all is given
//      (§19.6 final paragraph, "the warning is emitted unless the user
//      supplies `_`").

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

// True iff some diagnostic mentions non-exhaustiveness at Warning severity.
bool hasNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// --- Enum scrutinee (§19.6: "every declared constant has a matching
// pattern") -----------------------------------------------------------------

// Non-exhaustive enum case (missing a constant, no `_`) → WARNING, but NOT
// an error (the program still compiles; §19.6).
BEZEL_TEST(sema_case_exhaust, enum_missing_constant_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    var n := case c of "
        "      Red => 1, "
        "      Green => 2 "  // Blue uncovered, no `_`
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());  // warning, not error
}

// Exhaustive enum case (all constants covered) → no warning (§19.6 / §10.6).
BEZEL_TEST(sema_case_exhaust, enum_all_constants_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    var n := case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Enum case missing a constant BUT with a `_` arm → no warning. A catch-all
// is always exhaustive (§19.6 / §19.3 discard pattern).
BEZEL_TEST(sema_case_exhaust, enum_missing_with_discard_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    var n := case c of "
        "      Red => 1, "
        "      _ => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.6: comma-separated alternatives count toward coverage. `Red, Green`
// in one arm covers both; the third arm covers Blue → exhaustive.
BEZEL_TEST(sema_case_exhaust, enum_comma_alternatives_ok_spec_10_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    var n := case c of "
        "      Red | Green => 1, "
        "      Blue => 2 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An unconstrained `var x` (untyped) arm is a catch-all (§19.3 "Matches
// any value") — exhaustive even though a constant is missing.
BEZEL_TEST(sema_case_exhaust, enum_untyped_var_is_catchall_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color): int "
        "    var n := case c of "
        "      Red => 1, "
        "      var other => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A guarded arm covering the missing constant does NOT establish coverage —
// the guard can fail at runtime (§19.4). So this is still non-exhaustive.
BEZEL_TEST(sema_case_exhaust, enum_guarded_arm_does_not_cover_spec_19_4) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color, b: bool): int "
        "    var n := case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue when b => 3 "  // guarded — coverage of Blue not assured
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Nullable scrutinee (§19.6: "under the rules above plus an explicit
// `null` pattern") ----------------------------------------------------------

// A nullable enum scrutinee that covers every non-null constant but has no
// `null` arm is NON-exhaustive: the value can be null and no arm matches it.
BEZEL_TEST(sema_case_exhaust, nullable_enum_no_null_arm_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color?): int "
        "    var n := case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3 "  // every constant covered, but no `null` arm
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same nullable enum WITH an explicit `null` arm → exhaustive (§19.6).
BEZEL_TEST(sema_case_exhaust, nullable_enum_with_null_arm_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color?): int "
        "    var n := case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3, "
        "      null => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Bool scrutinee (§19.6: "both `true` and `false` are covered") ----------

BEZEL_TEST(sema_case_exhaust, bool_both_covered_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(b: bool): int "
        "    var n := case b of "
        "      true => 1, "
        "      false => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_case_exhaust, bool_one_missing_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(b: bool): int "
        "    var n := case b of "
        "      true => 1 "  // false uncovered, no `_`
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Nullable bool scrutinee (§19.6: bool rule + nullable-`null`-arm rule) ---
// fix #747 R43: a `bool?` covered by `null`, `true`, AND `false` is statically
// exhaustive; the checker previously skipped the bool branch for nullable
// scrutinees and drew a spurious non-exhaustiveness warning.

BEZEL_TEST(sema_case_exhaust, nullable_bool_null_true_false_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(b: bool?): int "
        "    var n := case b of "
        "      null => 2, "
        "      true => 1, "
        "      false => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `bool?` covering `true`+`false` but NOT `null` is non-exhaustive (§19.6:
// the nullable rule requires an explicit `null` pattern).
BEZEL_TEST(sema_case_exhaust, nullable_bool_no_null_arm_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(b: bool?): int "
        "    var n := case b of "
        "      true => 1, "
        "      false => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `bool?` with `null`+`true` but NOT `false` is non-exhaustive (the bool rule
// still requires both `true` and `false`).
BEZEL_TEST(sema_case_exhaust, nullable_bool_missing_false_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(b: bool?): int "
        "    var n := case b of "
        "      null => 2, "
        "      true => 1 "
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Sealed-abstract closed hierarchy (§11.7 / §19.6 example) ---------------

// The §19.6 spec example, adapted to integer fields (stdlib Math.Pi is real
// but uses doubles; we keep the structure — a `sealed abstract` Shape base
// with three sealed leaves, each matched by a `var c: T` pattern). Every
// leaf covered → exhaustive, no `_` needed.
BEZEL_TEST(sema_case_exhaust, sealed_hierarchy_all_leaves_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public override method Area(): int => 1; "
        "end Circle; "
        "sealed class Square : Shape "
        "  public override method Area(): int => 2; "
        "end Square; "
        "sealed class Triangle : Shape "
        "  public override method Area(): int => 3; "
        "end Triangle; "
        "class Calc "
        "  public method M(shape: Shape): int "
        "    var a := case shape of "
        "      var c: Circle => c.Area(), "
        "      var s: Square => s.Area(), "
        "      var t: Triangle => t.Area() "
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Non-exhaustive closed hierarchy (a leaf uncovered, no `_`) → WARNING.
BEZEL_TEST(sema_case_exhaust, sealed_hierarchy_missing_leaf_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public override method Area(): int => 1; "
        "end Circle; "
        "sealed class Square : Shape "
        "  public override method Area(): int => 2; "
        "end Square; "
        "sealed class Triangle : Shape "
        "  public override method Area(): int => 3; "
        "end Triangle; "
        "class Calc "
        "  public method M(shape: Shape): int "
        "    var a := case shape of "
        "      var c: Circle => c.Area(), "
        "      var s: Square => s.Area() "  // Triangle uncovered, no `_`
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A bare type-test pattern (`Circle => ...`, no binding) also counts toward
// leaf coverage (§19.3 "type patterns without binding").
BEZEL_TEST(sema_case_exhaust, sealed_hierarchy_bare_type_patterns_ok_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape end Shape; "
        "sealed class Circle : Shape end Circle; "
        "sealed class Square : Shape end Square; "
        "class Calc "
        "  public method M(shape: Shape): int "
        "    var a := case shape of "
        "      Circle => 1, "
        "      Square => 2 "
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Open hierarchy (§19.6 final paragraph) --------------------------------

// "When the hierarchy is open (the base is not sealed ...), exhaustiveness
// cannot be proven statically and the warning is emitted unless the user
// supplies `_`." A plain (non-sealed, non-abstract) base is open.
BEZEL_TEST(sema_case_exhaust, open_hierarchy_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class Calc "
        "  public method M(a: Animal): int "
        "    var n := case a of "
        "      Dog => 1, "
        "      Cat => 2 "  // open base: more subclasses could exist
        "    end; "
        "    return n; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same open hierarchy WITH a `_` arm → no warning (§19.6).
BEZEL_TEST(sema_case_exhaust, open_hierarchy_with_discard_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class Calc "
        "  public method M(a: Animal): int "
        "    var n := case a of "
        "      Dog => 1, "
        "      Cat => 2, "
        "      _ => 0 "
        "    end; "
        "    return n; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Open value-domain scrutinees (int) ------------------------------------

// An int scrutinee has no finite, statically-knowable value set; without a
// `_` arm the case is non-exhaustive (§19.6 general rule, line 3058).
BEZEL_TEST(sema_case_exhaust, int_scrutinee_no_discard_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(score: int): int "
        "    var n := case score of "
        "      90..100 => 1, "
        "      80..89 => 2 "  // no `_`: rest of the int range uncovered
        "    end; "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The §19.1 grade example: an int scrutinee WITH a `_` arm → exhaustive.
BEZEL_TEST(sema_case_exhaust, int_scrutinee_with_discard_ok_spec_19_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(score: int): string "
        "    var grade := case score of "
        "      90..100 => \"A\", "
        "      80..89 => \"B\", "
        "      70..<80 => \"C\", "
        "      _ => \"F\" "
        "    end; "
        "    return grade; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Statement position is exempt (§19.6) ----------------------------------

// A statement-position `case` with no `_` falls through silently on an
// unmatched value — NO warning, even when non-exhaustive (§19.6: "In
// statement position, an unmatched value falls through with no action").
BEZEL_TEST(sema_case_exhaust, statement_case_no_warning_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(c: Color) "
        "    case c of "
        "      Red => do return; end, "
        "      Green => do return; end "  // Blue uncovered — but statement position
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #784 / §19.5 / §19.6: an unguarded typed-binding `var n: T` arm matches
// all non-null values; with a `null` arm it makes a `T?` scrutinee exhaustive
// (sibling of the bool? fix). The checker wrongly warned because it treated
// only `_`/untyped `var x` as a catch-all.

// `null` + `var n: int` over int? → exhaustive, no warning.
BEZEL_TEST(sema_case_exhaust, nullable_typed_binding_plus_null_ok_spec_19_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int?): int "
        "    return case x of null => 0, var n: int => n end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A typed binding `var n: int` over a NON-nullable int covers everything.
BEZEL_TEST(sema_case_exhaust, nonnullable_typed_binding_is_exhaustive_spec_19_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int): int "
        "    return case x of var n: int => n end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
}

// A typed binding over int? WITHOUT a `null` arm is still non-exhaustive.
BEZEL_TEST(sema_case_exhaust, nullable_typed_binding_no_null_arm_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int?): int "
        "    return case x of var n: int => n end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
}

// A NARROWER typed binding (`var d: Dog` over `Animal`) does NOT cover all —
// still non-exhaustive without a catch-all (must not over-broaden).
BEZEL_TEST(sema_case_exhaust, narrower_typed_binding_still_warns_spec_19_5) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Cat: Animal end Cat; "
        "class C public static method M(a: Animal): int "
        "  return case a of var d: Dog => 1 end; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
}
