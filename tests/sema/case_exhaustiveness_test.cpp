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

// True iff a non-exhaustiveness warning also contains `needle` — used to assert
// §19.6's requirement that an undetermined-leaf warning NAMES the leaf and its
// undetermined parameter(s).
bool nonExhaustiveWarningMentions(const bezel::test::SemaResult& r,
                                  std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find(needle) != std::string::npos) {
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

// --- Generic leaves: the determination rule (§19.6, v0.196) ----------------
//
// §19.6 (v0.196) "Generic leaves: the determination rule", the "No inhabiting
// instantiation … ground mismatch ⇒ exempt" edge: when the closed hierarchy's
// root is generic, *leaf type* means the leaf INSTANTIATION admitted at the
// scrutinee's static type. A leaf whose composed root instantiation is a ground
// type distinct from the scrutinee's instantiation is UNINHABITED at S — "by
// invariance no value of static type `S` has an instantiation of `L` as its
// runtime class" — and is EXEMPT from coverage: "an exempt leaf needs no arm."
//
// `Box<T>` with `IntBox: Box<int>` and `StrBox: Box<string>`, scrutinee
// `Box<string>`: IntBox's root instantiation `Box<int>` is ground and unequal
// to `Box<string>`, so IntBox is exempt; covering StrBox alone is exhaustive.
// Before the s1 fix the name-match added IntBox to the leaf set and drew a
// spurious "missing leaf type(s): IntBox" warning.
BEZEL_TEST(sema_case_exhaust, generic_ground_mismatch_leaf_exempt_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Box<T> end Box; "
        "sealed class IntBox : Box<int> end IntBox; "
        "sealed class StrBox : Box<string> end StrBox; "
        "class Calc "
        "  public method M(b: Box<string>): int "
        "    var a := case b of "
        "      var s: StrBox => 0 "  // IntBox is uninhabited at Box<string>
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // IntBox exempt — no arm
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Companion: the exemption is gated on generics being in play, so a wholly
// non-generic closed hierarchy is UNAFFECTED — a genuinely-missing leaf still
// draws the warning (the gate-off name-match path is unchanged). This pins the
// guard that the s1 filter never suppresses a real missing-leaf diagnostic.
BEZEL_TEST(sema_case_exhaust,
           nongeneric_missing_leaf_unaffected_by_generic_filter_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Shape end Shape; "
        "sealed class Circle : Shape end Circle; "
        "sealed class Square : Shape end Square; "
        "sealed class Triangle : Shape end Triangle; "
        "class Calc "
        "  public method M(s: Shape): int "
        "    var a := case s of "
        "      var c: Circle => 1, "
        "      var q: Square => 2 "  // Triangle genuinely missing, no `_`
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));   // Triangle still required
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §19.6 (v0.196) generic-leaf DETERMINATION for a GROUND scrutinee (#1816
// s2) -----------------------------------------------------------------------
//
// "Generic leaves: the determination rule": for a ground scrutinee `S`, a
// leaf's exhaustiveness obligation is fixed by solving the leaf's parameters
// against `S` — its root instantiation matched against `S` component-wise,
// invariant, exact (§12.9; no §5.1 conversion, no variance). Empty solution set
// ⇒ uninhabited ⇒ EXEMPT; exactly one ⇒ DETERMINED, the sole admitted
// instantiation is the obligation; more than one / undetermined ⇒ a `_` is
// required. s1 handled only the ground-mismatch edge (a leaf with a *ground*
// root instantiation unequal to `S`); s2 solves the leaf's parameters, which
// covers the determined idiom and the repeated-parameter-inconsistency edge.

// (a) The canonical determined idiom (spec §19.6 example): `Result<T>` with
// `Ok<T>: Result<T>` and `Err<T>: Result<T>`, scrutinee `Result<int>`. Each
// leaf's root instantiation `Result<T>` matched against `Result<int>` solves
// `T := int` — the sole inhabiting instantiation — so `Ok<int>` and `Err<int>`
// are the only inhabitants and the two-arm case is exhaustive with no `_`. This
// is the accept shape the determination rule must NOT regress (the rejected
// "blanket never-exhaustive" reading would spuriously warn here).
BEZEL_TEST(sema_case_exhaust, determined_idiom_stays_exhaustive_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := case r of "
        "      var o: Ok<int> => 1, "   // T := int — the sole solution
        "      var e: Err<int> => 2 "   // T := int — the sole solution
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // both determined — no `_`
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b) THE s2 fails-without/passes-with delta. A repeated-parameter leaf
// `L<Y>: D<Y, Y>` under scrutinee `D<int, string>` has NO inhabiting
// instantiation: solving its root instantiation `D<Y, Y>` against
// `D<int, string>` binds `Y := int` then requires `Y := string` — inconsistent,
// the empty solution set (§19.6 "No inhabiting instantiation … `L<Y>: D<Y, Y>`
// against `D<int, string>`"). By invariance no value of static type
// `D<int, string>` has an `L<…>` runtime class, so `L` is EXEMPT; covering the
// ground leaf `P: D<int, string>` alone is exhaustive. Before s2 the solver did
// not run (L's root instantiation `D<Y, Y>` is non-ground, so s1's
// ground-mismatch slice left it in the leaf set) and the name-match drew a
// spurious "missing leaf type(s): L" warning.
BEZEL_TEST(sema_case_exhaust, repeated_param_inconsistent_leaf_exempt_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class D<A, B> end D; "
        "sealed class L<Y> : D<Y, Y> end L; "      // root inst D<Y,Y> — inconsistent
        "sealed class P : D<int, string> end P; "  // ground — the required arm
        "class Calc "
        "  public method M(d: D<int, string>): int "
        "    var a := case d of "
        "      var p: P => 1 "  // L is uninhabited at D<int, string>
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // L exempt — no arm needed
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) The determination rule must NEVER suppress a genuinely-missing determined
// leaf. Same `Result<T>` hierarchy and `Result<int>` scrutinee as (a) but the
// `Err<int>` arm is omitted: `Err` is DETERMINED at `Result<int>` (`T := int`,
// one solution) and left uncovered, so the case is non-exhaustive and the
// warning still fires — the sound instantiation-coverage reading, not the
// rejected "declaration-coverage" that would call a single leaf covered.
BEZEL_TEST(sema_case_exhaust, determined_missing_leaf_still_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := case r of "
        "      var o: Ok<int> => 1 "  // Err<int> determined but omitted
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));   // Err<int> still required
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (d) s3 — the UNDETERMINED free-parameter case (spec §19.6 example, v0.196).
// A leaf parameter with NO occurrence in the leaf's root instantiation is FREE
// after solving against the ground scrutinee: `Tagged<U>: Message` under a
// NON-GENERIC sealed `Message` — U never reaches the base clause `Message`, so
// Tagged<int>, Tagged<string>, … all inhabit Message (INFINITELY many). Leaf
// coverage is therefore statically UNATTAINABLE: a `Tagged<int>` arm names one
// instantiation, not the leaf, so the case is NOT exhaustive without an
// irrefutable arm, and §19.6 requires the warning to NAME the leaf and its
// undetermined parameter(s). This is the OPPOSITE of s2's NoSolution (0
// inhabitants ⇒ exempt): here every substitution inhabits. Fails without s3 —
// before it, the name-match counted `var g: Tagged<int>` as covering the
// `Tagged` leaf (the rejected "declaration-coverage" reading, §19.6) and no
// warning fired.
BEZEL_TEST(sema_case_exhaust, no_occurrence_param_undetermined_spec_19_6) {
    // No irrefutable arm: Tagged is undetermined ⇒ non-exhaustive, and the
    // warning names Tagged and its undetermined parameter U.
    auto warned = parseSemaAll(
        "module M; "
        "sealed abstract class Message end Message; "
        "sealed class Text : Message end Text; "
        "sealed class Tagged<U> : Message end Tagged; "  // U free — Message non-generic
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := case m of "
        "      var t: Text => 1, "
        "      var g: Tagged<int> => 2 "  // one instantiation — not the leaf
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(warned));      // Tagged unattainable
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "Tagged"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "undetermined"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "U"));
    BEZEL_EXPECT_FALSE(warned.diagnostics.hasErrors());

    // Same hierarchy WITH a `_` catch-all — the only remedy for an undetermined
    // leaf (§19.6): now exhaustive, no warning.
    auto cleared = parseSemaAll(
        "module M; "
        "sealed abstract class Message end Message; "
        "sealed class Text : Message end Text; "
        "sealed class Tagged<U> : Message end Tagged; "
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := case m of "
        "      var t: Text => 1, "
        "      var g: Tagged<int> => 2, "
        "      _ => 0 "
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(cleared));    // `_` covers the rest
    BEZEL_EXPECT_FALSE(cleared.diagnostics.hasErrors());
}

// --- s4: RIGID-scrutinee determination (spec §19.6 v0.196; design doc Tests 2
//     & 8) --------------------------------------------------------------------
//
// s1–s3 solve only a FULLY-GROUND scrutinee `S`. s4 handles the case they defer:
// `S` contains a rigid enclosing-body type parameter (`case r: Result<T>` inside
// a generic body, `T` rigid). Identity against a rigid parameter is three-valued
// (§19.6 v0.196): a leaf whose root instantiation equates a GROUND/COMPOSITE
// component against the rigid parameter is UNDECIDABLE at the declaration —
// neither exempt nor determined, coverage statically unattainable — while a leaf
// whose OWN parameter solves against the rigid one is determined (the solution
// mentions the rigid parameter and is expressible as an arm).

// (e) THE s4 fails-without/passes-with delta — the rigid-identity NEVER-EXEMPT
// case (design doc Test 8). `IntError: Result<int>` under scrutinee `Result<T>`
// (`T` rigid, from the enclosing `Handle<T>`): its root instantiation
// `Result<int>` matched against `Result<T>` equates ground `int` against the
// rigid `T` — inhabited exactly at the instantiations with `T = int`, undecidable
// at the declaration ⇒ statically UNATTAINABLE. The leaf is NEVER exempt and a
// specific-instantiation type-test arm (`var x: IntError`) does NOT discharge it;
// only an irrefutable arm does. Fails without s4: the name-match counted the
// `IntError` arm as covering the leaf (matching by declaration name) and no
// warning fired — a proof that `Handle<int>(new IntError())` falsifies at
// runtime. `Ok<Y>: Result<Y>` in the same hierarchy is DETERMINED (`Y := T`) and
// covered by `var o: Ok<T>`, so only `IntError` is the obligation.
BEZEL_TEST(sema_case_exhaust, rigid_result_leaf_never_exempt_spec_19_6) {
    // Both leaves have arms, no `_`: IntError is unattainable at rigid `Result<T>`
    // so the case is non-exhaustive and the warning NAMES IntError.
    auto warned = parseSemaAll(
        "module M; "
        "sealed abstract class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "       // determined: Y := T
        "sealed class IntError : Result<int> end IntError; "  // int ≡ T undecidable
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := case r of "
        "      var o: Ok<T> => 1, "        // Ok determined and covered
        "      var x: IntError => 2 "      // one instantiation — does NOT cover
        "    end; "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(warned));        // IntError unattainable
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "IntError"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "unattainable"));
    BEZEL_EXPECT_FALSE(warned.diagnostics.hasErrors());

    // The only remedy for a statically-unattainable rigid-identity leaf (§19.6):
    // an irrefutable `_` arm — now exhaustive, no warning.
    auto cleared = parseSemaAll(
        "module M; "
        "sealed abstract class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class IntError : Result<int> end IntError; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := case r of "
        "      var o: Ok<T> => 1, "
        "      var x: IntError => 2, "
        "      _ => 0 "
        "    end; "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(cleared));      // `_` completes it
    BEZEL_EXPECT_FALSE(cleared.diagnostics.hasErrors());
}

// (f) The determined RIGID shape stays exhaustive (design doc Test 2 / the §7
// migration note: "the determined shape — `Ok<T>/Err<T>: Result<T>` matched at …
// rigid `Result<T>` without `_`" is newly proven). `Ok<Y>: Result<Y>` and
// `Err<Y>: Result<Y>` under scrutinee `Result<T>` (rigid `T`): each leaf's root
// instantiation `Result<Y>` matched against `Result<T>` solves `Y := T` — the
// sole inhabiting instantiation — so `Ok<T>` and `Err<T>` are the only
// inhabitants and the two-arm case is exhaustive with no `_`. s4 must NOT
// regress the canonical generic-body sealed-match idiom into a warning ("blanket
// never-exhaustive" is the rejected reading); the solution mentions the rigid
// `T` and is expressible as the arm.
BEZEL_TEST(sema_case_exhaust, rigid_determined_leaf_stays_exhaustive_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class Err<Y> : Result<Y> end Err; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := case r of "
        "      var o: Ok<T> => 1, "        // T := T — the sole solution
        "      var e: Err<T> => 2 "        // T := T — the sole solution
        "    end; "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // both determined — no `_`
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- s5: leaf-chain scalar where-constraint filtering (spec §19.6 v0.196;
//     clause (b)) --------------------------------------------------------------
//
// §19.6 clause (b) *filters* clause (a)'s solutions: an inhabiting instantiation
// must also be a WELL-FORMED instantiation — the leaf's own `where` clauses and
// those of every base along its derivation chain are satisfied (§14.2). When
// every solution fails clause (b), "L is uninhabited at S … and is exempt."
// s5 implements the SCALAR fragment of clause (b): `class` / `value` / `notnull`
// / `new`, decidable against a ground solution (a type-bound `where Y: I` is s6).
//
// (g) THE s5 fails-without/passes-with delta. `L<Y>: B<Y> where Y: value` under
// scrutinee `B<string>`: clause (a) solves `Y := string` (the sole solution),
// but `string` is a reference type, so `where Y: value` (§14.2) CONTRADICTS it —
// the solution fails clause (b), the solution set empties, and L is UNINHABITED
// at `B<string>` ⇒ EXEMPT. Covering only the ground leaf `R: B<string>` is then
// exhaustive with no `_`. Fails without s5: before the filter the solver
// reported `Y := string` Determined and the name-match drew a spurious
// "missing leaf type(s): L" warning (the filter hole).
BEZEL_TEST(sema_case_exhaust, leaf_where_value_filters_solutions_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: value end L; "  // Y:=string ✗ value
        "sealed class R : B<string> end R; "               // ground — the arm
        "class Calc "
        "  public method M(b: B<string>): int "
        "    var a := case b of "
        "      var r: R => 1 "  // L exempt: string is not a value type
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // L filtered out — exempt
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (h) The COMPANION: the constraint is SATISFIED, so the filter must NOT drop
// the solution and the leaf stays REQUIRED. Same hierarchy but `where Y: class`
// — `string` IS a non-nullable reference type, so `Y := string` satisfies the
// constraint (§14.2), L is DETERMINED and inhabited at `B<string>`, and omitting
// its arm is non-exhaustive: the warning still fires, NAMING L. This is the
// guard the RISK note demands — s5 may only drop a PROVABLY-violating solution,
// never a satisfying one (that would wrongly exempt an inhabited leaf).
BEZEL_TEST(sema_case_exhaust,
           leaf_where_class_satisfied_keeps_leaf_required_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "sealed abstract class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: class end L; "  // Y:=string ✓ class
        "sealed class R : B<string> end R; "
        "class Calc "
        "  public method M(b: B<string>): int "
        "    var a := case b of "
        "      var r: R => 1 "  // L still required: string satisfies `class`
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));   // L inhabited — required
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "L"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- s6: type-bound where-constraint filtering (spec §19.6 v0.196; clause (b)) -
//
// s6 extends s5's clause-(b) filter to a TYPE bound `where Y: I` (§14.2): a
// SOLVED ground σ(Y) is kept only if σ(Y) is a subtype of the resolved bound,
// decided by declaration-based subtyping (ExprTC::isSubtype) on the ground
// solution. A solution that PROVABLY falls outside the bound is not a well-
// formed instantiation, so the singleton solution set empties and the leaf is
// EXEMPT; a satisfying (or undecidable) solution keeps the leaf REQUIRED.
//
// (a) THE s6 fails-without/passes-with delta, both directions. `L<Y>: B<Y>
// where Y: IShape`:
//   • at scrutinee `B<Square>` (Square: IShape) clause (a) solves `Y := Square`,
//     which SATISFIES `where Y: IShape` ⇒ L is DETERMINED and INHABITED; its
//     obligation `L<Square>` is present, so omitting its arm warns, NAMING L.
//   • at scrutinee `B<int>` clause (a) solves `Y := int`; `int` is not a subtype
//     of the user interface IShape (a primitive conforms only to the builtin
//     interfaces, §3.5/§8.6/stdlib §2.19) ⇒ `where Y: IShape` CONTRADICTS the
//     solution ⇒ L is UNINHABITED at `B<int>` and EXEMPT, so covering only the
//     ground leaf `Ri: B<int>` is exhaustive with no `_`.
// Fails without s6: the `type` case returned false (no filter), so at `B<int>`
// the solver reported `Y := int` Determined & inhabited and the name-match drew
// a spurious "missing leaf type(s): L" warning (the filter hole).
BEZEL_TEST(sema_case_exhaust, type_bound_filters_solved_param_spec_19_6) {
    // §19.6 v0.196 "Generic leaves: the determination rule" + declines D8.
    // Direction 1 — the bound is SATISFIED: L<Square> inhabited ⇒ REQUIRED
    // (guards against dropping a satisfying solution — a false exemption).
    auto present = parseSemaAll(
        "module M; "
        "interface IShape end IShape; "
        "class Square : IShape end Square; "
        "sealed abstract class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: IShape end L; "  // Y:=Square ✓ IShape
        "sealed class R : B<Square> end R; "                // ground leaf — arm
        "class Calc "
        "  public method M(b: B<Square>): int "
        "    var a := case b of "
        "      var r: R => 1 "  // L<Square> NOT covered → required
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(present));  // L<Square> inhabited
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(present, "L"));
    BEZEL_EXPECT_FALSE(present.diagnostics.hasErrors());

    // Direction 2 — the bound is PROVABLY VIOLATED: int is not IShape ⇒ L exempt.
    auto exempt = parseSemaAll(
        "module M; "
        "interface IShape end IShape; "
        "sealed abstract class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: IShape end L; "  // Y:=int ✗ IShape
        "sealed class Ri : B<int> end Ri; "                 // ground leaf at B<int>
        "class Calc "
        "  public method M(b: B<int>): int "
        "    var a := case b of "
        "      var r: Ri => 1 "  // L exempt: int is not a subtype of IShape
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(exempt));  // L<int> filtered out
    BEZEL_EXPECT_FALSE(exempt.diagnostics.hasErrors());
}

// (b) THE CENSUS PROHIBITION guard. `L<Z>: Message where Z: SealedC`, with Z
// having NO occurrence in the root instantiation `Message`. §19.6 v0.196: "a
// parameter of L with no occurrence in the root instantiation leaves the
// solution set unbounded WHATEVER that parameter's bounds, so a leaf with such a
// parameter is never exempt or determined by a census of a bound's conformers."
// SealedC is a `sealed` class with ZERO further conformers, so a conformer
// census would (wrongly) either mark L exempt (0 conformers ⇒ "uninhabited") or
// determine it (1 conformer ⇒ bind Z). The filter-only reading keeps Z FREE ⇒ L
// stays UNDETERMINED ⇒ the warning fires and NAMES L and its undetermined
// parameter Z. This FAILS if an implementer census-resolves the bound; PASSES
// with the filter-only reading — isSubtype is used ONLY on a ground σ(Z), never
// to enumerate SealedC's conformers.
BEZEL_TEST(sema_case_exhaust,
           sealed_bound_no_census_stays_undetermined_spec_19_6) {
    // §19.6 v0.196 "Zero-conformer / sealed bound must NOT be census-resolved"
    // + declines D8.
    auto r = parseSemaAll(
        "module M; "
        "sealed class SealedC end SealedC; "  // a sealed bound, zero conformers
        "sealed abstract class Message end Message; "
        "sealed class Text(body: string) : Message end Text; "  // ground leaf
        "sealed class L<Z>(z: Z) : Message where Z: SealedC end L; "  // Z free
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := case m of "
        "      var t: Text => 1 "  // L undetermined: Z never reaches the root
        "    end; "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));                // NOT census-resolved
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "L"));      // names the leaf
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "undetermined"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Z"));      // names its param
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
