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
//      `true` and `false`), a `closed` class hierarchy (every
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

// §21.7: true iff a diagnostic reports the "has a return type but does not
// return a value on every path" ERROR — i.e. §21.7's statement-`case`
// totality grant was WITHHELD for a value-returning method whose only
// return path is a statement-position `case` (every arm diverges, but the
// no-match fall-through was not proven statically dead).
bool hasReturnEveryPathError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not return a value on every path") !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}

// §19.3/§5.1/§12.9 (#1913): true iff an ERROR reports the ADMISSION rejection of
// a wrong-instantiation type-test binding. A GROUND wrong-instantiation arm
// (`var x: Ok<string>` at `Result<int>`) is not merely non-covering (#1874, the
// WARNING below) — it is a statically-impossible bind that #1913 rejects at
// admission. The two diagnostics co-occur on such a program: the warning still
// fires (proving #1874's arg-sensitive coverage is unchanged and NAMES the
// uncovered leaf), and the error additionally rejects the impossible bind. A
// RIGID wrong-instantiation arm draws only the warning — admission accepts it
// (a type pattern is tested per instantiation, §19.3 v0.182).
bool hasWrongInstantiationAdmissionError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("generic instantiation can never match") !=
                std::string::npos)
            return true;
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
        "    var n := (case c of "
        "      Red => 1, "
        "      Green => 2 "  // Blue uncovered, no `_`
        "); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      _ => 0); "
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
        "    var n := (case c of "
        "      Red | Green => 1, "
        "      Blue => 2); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      var other => 0); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue when b => 3 "  // guarded — coverage of Blue not assured
        "); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3 "  // every constant covered, but no `null` arm
        "); "
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
        "    var n := (case c of "
        "      Red => 1, "
        "      Green => 2, "
        "      Blue => 3, "
        "      null => 0); "
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
        "    var n := (case b of "
        "      true => 1, "
        "      false => 0); "
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
        "    var n := (case b of "
        "      true => 1 "  // false uncovered, no `_`
        "); "
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
        "    var n := (case b of "
        "      null => 2, "
        "      true => 1, "
        "      false => 0); "
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
        "    var n := (case b of "
        "      true => 1, "
        "      false => 0); "
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
        "    var n := (case b of "
        "      null => 2, "
        "      true => 1); "
        "    return n; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Sealed-abstract closed hierarchy (§11.7 / §19.6 example) ---------------

// The §19.6 spec example, adapted to integer fields (stdlib Math.Pi is real
// but uses doubles; we keep the structure — a `closed` Shape base
// with three sealed leaves, each matched by a `var c: T` pattern). Every
// leaf covered → exhaustive, no `_` needed.
BEZEL_TEST(sema_case_exhaust, sealed_hierarchy_all_leaves_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Shape "
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
        "    var a := (case shape of "
        "      var c: Circle => c.Area(), "
        "      var s: Square => s.Area(), "
        "      var t: Triangle => t.Area()); "
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
        "closed class Shape "
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
        "    var a := (case shape of "
        "      var c: Circle => c.Area(), "
        "      var s: Square => s.Area() "  // Triangle uncovered, no `_`
        "); "
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
        "closed class Shape end Shape; "
        "sealed class Circle : Shape end Circle; "
        "sealed class Square : Shape end Square; "
        "class Calc "
        "  public method M(shape: Shape): int "
        "    var a := (case shape of "
        "      Circle => 1, "
        "      Square => 2); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// §19.6 / #4195 (v0.298): an UNSUBSTITUTED generic type parameter constrained
// to a closed/sealed hierarchy (`where T: Animal`, with `Animal` `closed` and
// its subclasses `sealed`) is a `Named` TypeRef whose `declaration` is null —
// the rigid-param shape `matchLeafRigid` keys on. The pre-#4195 closed-
// hierarchy bullet gated on `scrutTy->declaration != nullptr`, so a `case`
// over such a scrutinee fell through to the generic "cannot prove" path and
// was falsely warned (expression position) — or, in statement position,
// folded the no-match edge into the ownership arm-join as if it were live,
// falsely rejecting a subsequent reassignment/dispose of a local every arm
// consumed (the §28.3 K1.4 false reject the issue documents on its repro).
//
// The fix: `caseArmsExhaustive` resolves the parameter's declared constraint
// bound through the same `forEachConstraintBound` walk the static-interface-
// operator lookup uses, so the closed-hierarchy bullet re-runs against the
// constraint's declaration when the scrutinee itself is the bare parameter.
// =============================================================================

// Generic type parameter `T` constrained to `closed Animal` with every
// subclass `sealed` — every leaf covered, NO wildcard → no warning
// (FAIL-WITHOUT on #4195: "scrutinee type cannot be proven exhaustive",
// §19.6; PASS-WITH: clean).
BEZEL_TEST(sema_case_exhaust,
           generic_typeparam_constrained_to_closed_hierarchy_all_leaves_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Animal end Animal; "
        "sealed class Dog: Animal end Dog; "
        "sealed class Cat: Animal end Cat; "
        "class C "
        "  public static method M<T>(v: T): int where T: Animal "
        "    return (case v of "
        "      var d: Dog => 1, "
        "      var c: Cat => 2); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Reject-side control (#4195 §15): same shape but a leaf UNCOVERED — must
// still warn. Pins that the constraint-bound fallback does not also swallow a
// genuinely non-exhaustive case (the safe-direction concern).
BEZEL_TEST(sema_case_exhaust,
           generic_typeparam_constrained_to_closed_hierarchy_missing_leaf_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Animal end Animal; "
        "sealed class Dog: Animal end Dog; "
        "sealed class Cat: Animal end Cat; "
        "class C "
        "  public static method M<T>(v: T): int where T: Animal "
        "    return (case v of "
        "      var d: Dog => 1); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());  // warning, not error
}

// Reject-side control (#4195 §15): a NON-CLOSED base class with two
// non-sealed subclasses — even when the constraint makes the parameter's
// value space "open, but bounded by this base", the §11.7 closed-set
// guarantee is absent so the case cannot be proven exhaustive without a
// catch-all. The constraint-bound fallback must inherit this same
// strictness: a non-`closed` / non-recursively-sealed base, even when it is
// the only bound on `T`, is NOT a closed-value-space hierarchy and so still
// warns.
BEZEL_TEST(sema_case_exhaust,
           generic_typeparam_constrained_to_open_hierarchy_still_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Cat: Animal end Cat; "
        "class C "
        "  public static method M<T>(v: T): int where T: Animal "
        "    return (case v of "
        "      var d: Dog => 1, "
        "      var c: Cat => 2); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Interface-only constraint (#4195 §15): a `where T: IFoo` where IFoo is
// an INTERFACE has no closed-value-space guarantee — the constraint-bound
// fallback must reject the interface case and fall through to the generic
// "cannot prove" path, NOT silently treat the interface declaration as a
// closed hierarchy. A `case` over such a scrutinee with no wildcard still
// warns.
BEZEL_TEST(sema_case_exhaust,
           generic_typeparam_constrained_to_interface_only_still_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "interface IThing end IThing; "
        "class C "
        "  public static method M<T>(v: T): int where T: IThing "
        "    return (case v of "
        "      var d: IThing => 1); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
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
        "closed class Box<T> end Box; "
        "sealed class IntBox : Box<int> end IntBox; "
        "sealed class StrBox : Box<string> end StrBox; "
        "class Calc "
        "  public method M(b: Box<string>): int "
        "    var a := (case b of "
        "      var s: StrBox => 0 "  // IntBox is uninhabited at Box<string>
        "); "
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
        "closed class Shape end Shape; "
        "sealed class Circle : Shape end Circle; "
        "sealed class Square : Shape end Square; "
        "sealed class Triangle : Shape end Triangle; "
        "class Calc "
        "  public method M(s: Shape): int "
        "    var a := (case s of "
        "      var c: Circle => 1, "
        "      var q: Square => 2 "  // Triangle genuinely missing, no `_`
        "); "
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
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "   // T := int — the sole solution
        "      var e: Err<int> => 2 "   // T := int — the sole solution
        "); "
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
        "closed class D<A, B> end D; "
        "sealed class L<Y> : D<Y, Y> end L; "      // root inst D<Y,Y> — inconsistent
        "sealed class P : D<int, string> end P; "  // ground — the required arm
        "class Calc "
        "  public method M(d: D<int, string>): int "
        "    var a := (case d of "
        "      var p: P => 1 "  // L is uninhabited at D<int, string>
        "); "
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
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1 "  // Err<int> determined but omitted
        "); "
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
        "closed class Message end Message; "
        "sealed class Text : Message end Text; "
        "sealed class Tagged<U> : Message end Tagged; "  // U free — Message non-generic
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := (case m of "
        "      var t: Text => 1, "
        "      var g: Tagged<int> => 2 "  // one instantiation — not the leaf
        "); "
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
        "closed class Message end Message; "
        "sealed class Text : Message end Text; "
        "sealed class Tagged<U> : Message end Tagged; "
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := (case m of "
        "      var t: Text => 1, "
        "      var g: Tagged<int> => 2, "
        "      _ => 0); "
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
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "       // determined: Y := T
        "sealed class IntError : Result<int> end IntError; "  // int ≡ T undecidable
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var o: Ok<T> => 1, "        // Ok determined and covered
        "      var x: IntError => 2 "      // one instantiation — does NOT cover
        "); "
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
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class IntError : Result<int> end IntError; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var o: Ok<T> => 1, "
        "      var x: IntError => 2, "
        "      _ => 0); "
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
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class Err<Y> : Result<Y> end Err; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var o: Ok<T> => 1, "        // T := T — the sole solution
        "      var e: Err<T> => 2 "        // T := T — the sole solution
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // both determined — no `_`
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- #1862: ARG-SENSITIVE coverage (spec §19.6 v0.196) ----------------------
//
// The determination rule fixes a determined leaf's obligation to its SOLE
// admitted instantiation and requires it "covered exactly as a non-generic leaf
// is — by an unguarded type pattern it is assignable to". Coverage is therefore
// arg-SENSITIVE: a WRONG-instantiation arm does NOT discharge a determined
// leaf's obligation. The rejected "declaration-coverage" reading (§19.6: "is
// unsound … an `Ok<string>` value matches no arm and raises
// MatchFailureException") matched an arm to a leaf by declaration NAME alone,
// wrongly deeming a non-exhaustive match exhaustive (the false-exhaustive hole,
// the UNSOUND direction). These pin the arg-sensitive coverage match.

// (a) GROUND wrong-instantiation. Scrutinee `Result<int>`; `Ok` is DETERMINED
// (`T := int`) so its obligation is `Ok<int>`, but the arm is `Ok<string>` —
// which is UNINHABITED at `Result<int>` (`Ok<string>` derives `Result<string>`,
// not `Result<int>`) and does NOT cover `Ok<int>`. With `Err<int>` covered,
// `Ok<int>` is uncovered and the case is non-exhaustive. FAILS WITHOUT the fix:
// name-match discharges `Ok` by declaration name, wrongly deeming the case
// exhaustive (no warning). PASSES WITH: the arg-sensitive check leaves `Ok<int>`
// uncovered and the warning names the leaf.
BEZEL_TEST(sema_case_exhaust,
           wrong_instantiation_ground_arm_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var a2: Ok<string> => 1, "  // WRONG inst — does NOT cover Ok<int>
        "      var e: Err<int> => 2 "      // T := int — covers Err<int>
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));            // Ok<int> uncovered
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));  // names the leaf
    // #1913: the GROUND wrong-instantiation arm `var a2: Ok<string>` is ALSO a
    // hard admission error now (a statically-impossible bind), reported
    // alongside the coverage warning above. #1874's arg-sensitive coverage is
    // unchanged — the warning still fires and names the leaf.
    BEZEL_EXPECT_TRUE(hasWrongInstantiationAdmissionError(r));
}

// (b) RIGID wrong-instantiation. Scrutinee `Result<T>` (rigid `T`, from the
// enclosing `Handle<T>`); `Ok<Y>: Result<Y>` is DETERMINED (`Y := T`) so its
// obligation is `Ok<T>`, but the arm is `Ok<int>` — one instantiation of the
// rigid family, not the determined `Ok<T>`. It does NOT cover the obligation, so
// with `Err<T>` covered the case is non-exhaustive. FAILS WITHOUT the fix
// (name-match discharges `Ok`); PASSES WITH (arg-sensitive: `int` ≠ rigid `T`).
BEZEL_TEST(sema_case_exhaust,
           wrong_instantiation_rigid_arm_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class Err<Y> : Result<Y> end Err; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "   // WRONG inst — does NOT cover Ok<T>
        "      var e: Err<T> => 2 "     // Y := T — covers Err<T>
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));            // Ok<T> uncovered
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));  // names the leaf
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) NO-OVER-WARN control (both directions). A CORRECTLY-instantiated arm MUST
// still cover its determined leaf — the arg-sensitive check must not regress the
// determined idiom. `Ok<int>` + `Err<int>` at `Result<int>` are each the sole
// admitted instantiation and each covered, so the case is exhaustive with no
// `_` and no warning (and no error). Guards the opposite failure mode of the
// #1862 fix: a spurious missing-leaf warning on a legitimately-covered leaf.
BEZEL_TEST(sema_case_exhaust,
           correct_instantiation_control_stays_covered_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "   // T := int — covers Ok<int>
        "      var e: Err<int> => 2 "   // T := int — covers Err<int>
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // both correctly covered
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- #1874: ARG-SENSITIVE coverage via an ANCESTOR arm (spec §19.6 v0.196) ---
//
// #1862 made a determined leaf's DIRECT self-match arg-sensitive; this closes the
// residual it left in the ANCESTOR/subtype-cover path. §19.6 requires a
// determined leaf `L<σ>` "covered … by an unguarded type pattern it is assignable
// TO", so an ancestor arm covers `L<σ>` only when `L<σ>` reference-converts to the
// arm's type (§5.1) — arg-SENSITIVE, class hierarchies being invariant (§12.9). A
// WRONG-instantiation ancestor arm (`var x: Result<string>` at scrutinee
// `Result<int>`) does NOT discharge the determined `Ok<int>`/`Err<int>`. The
// rejected "declaration-coverage" reading (§19.6, unsound) matched the ancestor to
// the leaf by declaration NAME alone through the subtype walk, wrongly deeming a
// non-exhaustive match exhaustive (the false-exhaustive hole).

// (a) GROUND wrong-instantiation ROOT ancestor. Scrutinee `Result<int>`; the sole
// arm `var x: Result<string>` names the root Result with the WRONG argument. At
// runtime the erased type-test catches every value (§19.5), but STATICALLY
// `Result<int>` is not assignable to `Result<string>` (invariant), so neither
// determined leaf `Ok<int>`/`Err<int>` is covered and the case is non-exhaustive.
// FAILS WITHOUT the fix: the #784 typed-binding-covers-full-type check matched by
// declaration alone (`Result` == `Result`) and returned "exhaustive" with no
// warning. PASSES WITH: the arg-sensitive check leaves both leaves uncovered.
BEZEL_TEST(sema_case_exhaust,
           wrong_instantiation_ancestor_root_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var x: Result<string> => 0 "  // WRONG-inst ANCESTOR — covers nothing
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));            // leaves uncovered
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));  // names the leaf
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Err"));
    // #1913: the GROUND wrong-instantiation ROOT ancestor `var x: Result<string>`
    // is ALSO a hard admission error (a statically-impossible bind), reported
    // alongside the coverage warning above.
    BEZEL_EXPECT_TRUE(hasWrongInstantiationAdmissionError(r));
}

// (b) CORRECT-instantiation ROOT ancestor control. `var x: Result<int>` at
// scrutinee `Result<int>` DOES cover every value — each determined leaf `Ok<int>`
// / `Err<int>` is assignable to `Result<int>` (an upcast, §5.1) — so the case is
// exhaustive with no `_` and no warning. Guards the opposite failure mode: the
// arg-sensitive ancestor check must not regress the legitimate "root binding
// covers all leaves" idiom.
BEZEL_TEST(sema_case_exhaust,
           correct_instantiation_ancestor_root_covers_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var x: Result<int> => 0 "  // CORRECT-inst ANCESTOR — covers all
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // root binding covers all
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) GROUND wrong-instantiation INTERMEDIATE-abstract ancestor. Scrutinee
// `Result<int>`; the arm names an intermediate abstract `Wrap<string>` (leaves
// `Ok`/`Err` derive `Wrap`). This cover flows through the closed-hierarchy leaf
// subtype-walk (not the #784 self-declaration check, since `Wrap` != `Result`), so
// it exercises the SECOND arg-sensitivity site. `Ok<int>`/`Err<int>` project up to
// `Wrap<int>`, not `Wrap<string>`, so neither is covered → non-exhaustive. FAILS
// WITHOUT (the subtype walk covered by ancestor-declaration name); PASSES WITH.
BEZEL_TEST(sema_case_exhaust,
           wrong_instantiation_ancestor_intermediate_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "closed class Wrap<T> : Result<T> end Wrap; "
        "sealed class Ok<T> : Wrap<T> end Ok; "
        "sealed class Err<T> : Wrap<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var w: Wrap<string> => 0 "  // WRONG-inst INTERMEDIATE — covers nothing
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Err"));
    // #1913: the GROUND wrong-instantiation INTERMEDIATE ancestor
    // `var w: Wrap<string>` is ALSO a hard admission error (a statically-
    // impossible bind), reported alongside the coverage warning above.
    BEZEL_EXPECT_TRUE(hasWrongInstantiationAdmissionError(r));
}

// (d) RIGID wrong-instantiation ROOT ancestor. Scrutinee `Result<T>` (rigid `T`,
// from the enclosing `Handle<T>`); the arm `var x: Result<int>` names one ground
// instantiation of the rigid family. `Result<T>` reference-converts to
// `Result<int>` only at the instantiation `T = int`, not at EVERY instantiation of
// the enclosing body (§31.2.1), so the binding does NOT universally cover and the
// case is non-exhaustive. FAILS WITHOUT (variance treated the rigid `T` as a free
// variable compatible with `int`); PASSES WITH (the rigid-mismatch guard).
BEZEL_TEST(sema_case_exhaust,
           wrong_instantiation_ancestor_rigid_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class Err<Y> : Result<Y> end Err; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var x: Result<int> => 0 "  // one instantiation — not the rigid family
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (e) RIGID CORRECT-instantiation ROOT ancestor control. `var x: Result<T>` at
// rigid scrutinee `Result<T>` covers every value (identity), so the case is
// exhaustive — the rigid-mismatch guard must accept the SAME rigid parameter.
BEZEL_TEST(sema_case_exhaust,
           correct_instantiation_ancestor_rigid_covers_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<A> end Result; "
        "sealed class Ok<Y> : Result<Y> end Ok; "
        "sealed class Err<Y> : Result<Y> end Err; "
        "class Handle<T> "
        "  public method h(r: Result<T>): int "
        "    var a := (case r of "
        "      var x: Result<T> => 0 "  // SAME rigid family — covers all
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // rigid identity covers all
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (f) NON-GENERIC base-binding control. A `var x: Shape` binding over a non-generic
// closed hierarchy still covers every leaf (the #784 typed-binding idiom). Guards
// that the #1874 generic arg-sensitivity did not disturb the non-generic path.
BEZEL_TEST(sema_case_exhaust,
           nongeneric_base_binding_still_covers_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Shape end Shape; "
        "sealed class Circle : Shape end Circle; "
        "sealed class Square : Shape end Square; "
        "class Calc "
        "  public method M(s: Shape): int "
        "    var a := (case s of "
        "      var x: Shape => 0 "  // base binding — covers all non-null values
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- #1912: CROSS-DECLARATION ancestor arm — the #784 full-type block (spec
//     §19.6 v0.196) --------------------------------------------------------------
//
// #1874 made the #784 "typed binding covers the WHOLE scrutinee type" check
// arg-SENSITIVE for the SAME-declaration disjunct (scrutinee `Result<int>`, arm
// `var x: Result<string>`). #1912 closes the residual its CROSS-declaration
// disjunct left: when the arm names a proper class-chain ANCESTOR of the
// scrutinee's declaration (scrutinee `Wrap<int>`/`Wrap<T>`, arm `Result<…>`), the
// check discharged coverage by declaration NAME alone (`isSubtype(Wrap, Result)`),
// ignoring the generic-argument mismatch. Per §19.6 v0.196 a typed binding covers
// every value only when the scrutinee is ASSIGNABLE to the arm's type (§5.1) —
// arg-SENSITIVE, class hierarchies being invariant (§12.9). The fix projects the
// scrutinee up the class chain to the arm's declaration and compares
// arg-sensitively, mirroring the same-declaration branch. Distinct from the
// #1874 (a)-(e) tests, whose scrutinee is the ROOT `Result` — here the scrutinee
// is a proper SUBTYPE, so `isSubtype(scrutinee-decl, arm-decl)` is TRUE and the
// #784 CROSS-declaration disjunct (not the leaf analysis) is what over-covered.

// (#1912 a) GROUND wrong-instantiation cross-declaration ancestor. Scrutinee
// `Wrap<int>` (an intermediate-abstract SUBTYPE of `Result`); the sole arm
// `var x: Result<string>` names the ancestor `Result` with the WRONG argument.
// `Wrap<int>` is not assignable to `Result<string>` (invariant, §12.9), so neither
// determined leaf `Ok<int>`/`Err<int>` is covered and the case is non-exhaustive.
// FAILS WITHOUT the fix: the #784 cross-declaration disjunct matched by
// declaration alone (`isSubtype(Wrap, Result)`) and returned "exhaustive" with no
// warning. PASSES WITH: the arg-sensitive projection leaves both leaves uncovered.
BEZEL_TEST(sema_case_exhaust,
           crossdecl_wrong_instantiation_ancestor_ground_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "closed class Wrap<T> : Result<T> end Wrap; "
        "sealed class Ok<T> : Wrap<T> end Ok; "
        "sealed class Err<T> : Wrap<T> end Err; "
        "class Calc "
        "  public method M(w: Wrap<int>): int "
        "    var a := (case w of "
        "      var x: Result<string> => 0 "  // WRONG-inst cross-decl ANCESTOR
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));            // leaves uncovered
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));  // names the leaf
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Err"));
    // #1913: the GROUND wrong-instantiation ancestor is ALSO a hard admission
    // error (a statically-impossible bind), reported alongside the warning.
    BEZEL_EXPECT_TRUE(hasWrongInstantiationAdmissionError(r));
}

// (#1912 b) GROUND correct-instantiation cross-declaration ancestor control.
// `var x: Result<int>` at scrutinee `Wrap<int>` DOES cover every value — an upcast
// (§5.1) — so the case is exhaustive with no `_` and no warning. Guards the
// opposite failure mode: the arg-sensitive cross-declaration projection must not
// regress the legitimate "ancestor binding covers all leaves" idiom.
BEZEL_TEST(sema_case_exhaust,
           crossdecl_correct_instantiation_ancestor_ground_covers_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "closed class Wrap<T> : Result<T> end Wrap; "
        "sealed class Ok<T> : Wrap<T> end Ok; "
        "sealed class Err<T> : Wrap<T> end Err; "
        "class Calc "
        "  public method M(w: Wrap<int>): int "
        "    var a := (case w of "
        "      var x: Result<int> => 0 "  // CORRECT-inst cross-decl ANCESTOR
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // ancestor binding covers all
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (#1912 c) RIGID wrong-instantiation cross-declaration ancestor. Scrutinee
// `Wrap<T>` (rigid `T`, from the enclosing `Handle<T>`); the arm
// `var x: Result<string>` names ONE ground instantiation of the ancestor family.
// `Wrap<T>` reference-converts to `Result<string>` only at `T = string`, not at
// EVERY instantiation of the enclosing body (§31.2.1), so the binding does NOT
// universally cover and the case is non-exhaustive — yet admission ACCEPTS it (a
// type pattern is tested per instantiation, §19.3 v0.182), so NO error is raised.
// FAILS WITHOUT (the cross-declaration disjunct covered by ancestor-declaration
// name); PASSES WITH (the projected rigid-mismatch guard).
BEZEL_TEST(sema_case_exhaust,
           crossdecl_wrong_instantiation_ancestor_rigid_does_not_cover_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<A> end Result; "
        "closed class Wrap<B> : Result<B> end Wrap; "
        "sealed class Ok<Y> : Wrap<Y> end Ok; "
        "sealed class Err<Y> : Wrap<Y> end Err; "
        "class Handle<T> "
        "  public method h(w: Wrap<T>): int "
        "    var a := (case w of "
        "      var x: Result<string> => 0 "  // one instantiation — not the family
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Ok"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());  // rigid bind is per-instantiation
}

// (#1912 d) RIGID correct-instantiation cross-declaration ancestor control.
// `var x: Result<T>` at rigid scrutinee `Wrap<T>` covers every value (projected
// identity `Result<T>`), so the case is exhaustive — the projected rigid-mismatch
// guard must accept the SAME rigid parameter.
BEZEL_TEST(sema_case_exhaust,
           crossdecl_correct_instantiation_ancestor_rigid_covers_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<A> end Result; "
        "closed class Wrap<B> : Result<B> end Wrap; "
        "sealed class Ok<Y> : Wrap<Y> end Ok; "
        "sealed class Err<Y> : Wrap<Y> end Err; "
        "class Handle<T> "
        "  public method h(w: Wrap<T>): int "
        "    var a := (case w of "
        "      var x: Result<T> => 0 "  // SAME rigid family — covers all
        "); "
        "    return a; "
        "  end h; "
        "end Handle;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // rigid identity covers all
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
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: value end L; "  // Y:=string ✗ value
        "sealed class R : B<string> end R; "               // ground — the arm
        "class Calc "
        "  public method M(b: B<string>): int "
        "    var a := (case b of "
        "      var r: R => 1 "  // L exempt: string is not a value type
        "); "
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
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: class end L; "  // Y:=string ✓ class
        "sealed class R : B<string> end R; "
        "class Calc "
        "  public method M(b: B<string>): int "
        "    var a := (case b of "
        "      var r: R => 1 "  // L still required: string satisfies `class`
        "); "
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
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: IShape end L; "  // Y:=Square ✓ IShape
        "sealed class R : B<Square> end R; "                // ground leaf — arm
        "class Calc "
        "  public method M(b: B<Square>): int "
        "    var a := (case b of "
        "      var r: R => 1 "  // L<Square> NOT covered → required
        "); "
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
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y> where Y: IShape end L; "  // Y:=int ✗ IShape
        "sealed class Ri : B<int> end Ri; "                 // ground leaf at B<int>
        "class Calc "
        "  public method M(b: B<int>): int "
        "    var a := (case b of "
        "      var r: Ri => 1 "  // L exempt: int is not a subtype of IShape
        "); "
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
        "closed class Message end Message; "
        "sealed class Text(body: string) : Message end Text; "  // ground leaf
        "sealed class L<Z>(z: Z) : Message where Z: SealedC end L; "  // Z free
        "class Calc "
        "  public method M(m: Message): int "
        "    var a := (case m of "
        "      var t: Text => 1 "  // L undetermined: Z never reaches the root
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));                // NOT census-resolved
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "L"));      // names the leaf
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "undetermined"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Z"));      // names its param
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- s7: §7.10 `?`-idempotence pre-image split (spec §19.6 + §7.10 v0.196) ---
//
// `?` is the one non-injective type constructor: `(U?)? ≡ U?` (§7.10). So a `Y?`
// occurrence in a leaf's root instantiation matched against a NULLABLE scrutinee
// argument `U?` has a finite pre-image SET — BOTH `Y := U` (`U? = U?`) and
// `Y := U?` (`(U?)? = U?`) inhabit. Two solutions ⇒ the leaf is UNDETERMINED
// (coverage statically unattainable). A §14.2 constraint on Y that excludes
// nullable arguments (`notnull` / `class` / `value` / a type or type-parameter
// bound) kills the nullable pre-image `Y := U?`, collapsing the split to the
// single non-null pre-image ⇒ DETERMINED (or, if that pre-image also violates a
// bound, EXEMPT). `new` is NOT such a constraint (a nullable type satisfies it
// via its `null` default).

// (a) THE s7 fails-without/passes-with delta — the UNCONSTRAINED split ⇒
// UNDETERMINED. `L<Y>: B<Y?>` under scrutinee `B<int?>`: the occurrence `Y?`
// matched against `int?` admits `Y := int` AND `Y := int?` (§7.10) — two
// inhabiting instantiations, so `L` coverage is statically unattainable and the
// case is NOT exhaustive even WITH a `var l: L<int>` arm (that names one
// instantiation, not the leaf). §19.6 requires the warning to NAME the leaf and
// its undetermined parameter. Fails without s7: the `Y?` occurrence returned
// Unknown (undetermined with no free parameter), so the leaf was kept and the
// `var l: L<int>` name-match counted it covered ⇒ no warning (the rejected
// declaration-coverage reading) — a proof that `L<int?>` matches no arm.
BEZEL_TEST(sema_case_exhaust, qmark_split_unconstrained_undetermined_spec_19_6) {
    // §7.10 / §19.6 v0.196. Unconstrained Y ⇒ both pre-images inhabit ⇒
    // undetermined; the warning names L and Y despite the `var l: L<int>` arm.
    auto warned = parseSemaAll(
        "module M; "
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y?> end L; "   // Y? occurrence — the §7.10 split
        "sealed class R : B<int?> end R; "    // ground leaf — determined, covered
        "class Calc "
        "  public method M(b: B<int?>): int "
        "    var a := (case b of "
        "      var r: R => 1, "
        "      var l: L<int> => 2 "  // one instantiation — does NOT cover the leaf
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(warned));        // L undetermined
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "L"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "undetermined"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "Y"));
    BEZEL_EXPECT_FALSE(warned.diagnostics.hasErrors());

    // The only remedy for an undetermined split leaf (§19.6): an irrefutable arm.
    auto cleared = parseSemaAll(
        "module M; "
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y?> end L; "
        "sealed class R : B<int?> end R; "
        "class Calc "
        "  public method M(b: B<int?>): int "
        "    var a := (case b of "
        "      var r: R => 1, "
        "      var l: L<int> => 2, "
        "      _ => 0); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(cleared));      // `_` completes it
    BEZEL_EXPECT_FALSE(cleared.diagnostics.hasErrors());
}

// (b) THE COMPANION — a §14.2 constraint on Y that excludes nullable arguments
// SUPPRESSES the nullable pre-image, collapsing the split back to a single
// solution. Two directions:
//   • `where Y: notnull` under `B<int?>`: the nullable pre-image `Y := int?` is
//     excluded (§14.2 notnull ⇒ non-null Y), leaving only `Y := int` ⇒ L is
//     DETERMINED and covered by the `var l: L<int>` arm ⇒ NO warning.
//   • `where Y: IShape` under `B<int?>` (int not IShape): the nullable pre-image
//     `Y := int?` is excluded (a type bound implies non-null Y), AND the non-null
//     pre-image `Y := int` violates the same bound (int is not a subtype of the
//     user interface IShape, §3.5) ⇒ BOTH pre-images fail ⇒ L is EXEMPT, so
//     covering only the ground leaf `R` is exhaustive with no `_` and no L arm.
// The second direction FAILS WITHOUT s7 (and s5/s6 alone): before s7 the `Y?`
// occurrence returned Unknown, so L was kept, uncovered, and drew a spurious
// missing-leaf warning; the constraint could not collapse a split the solver
// never recognized.
BEZEL_TEST(sema_case_exhaust, qmark_split_constraint_suppresses_nullable_spec_19_6) {
    // §7.10 / §19.6 v0.196 — notnull collapses the split to the DETERMINED
    // non-null pre-image, covered by the arm ⇒ no warning.
    auto determined = parseSemaAll(
        "module M; "
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y?> where Y: notnull end L; "  // kills Y := int?
        "sealed class R : B<int?> end R; "
        "class Calc "
        "  public method M(b: B<int?>): int "
        "    var a := (case b of "
        "      var r: R => 1, "
        "      var l: L<int> => 2 "  // covers the sole (determined) obligation
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(determined));   // determined+covered
    BEZEL_EXPECT_FALSE(determined.diagnostics.hasErrors());

    // §7.10 / §19.6 v0.196 — a type bound both suppresses the nullable pre-image
    // AND (here) rejects the non-null one ⇒ L EXEMPT ⇒ no warning without an L
    // arm. Fails without s7: L kept + uncovered ⇒ spurious missing-leaf warning.
    auto exempt = parseSemaAll(
        "module M; "
        "interface IShape end IShape; "
        "closed class B<T> end B; "
        "sealed class L<Y> : B<Y?> where Y: IShape end L; "   // int? and int both ✗
        "sealed class R : B<int?> end R; "                    // the only obligation
        "class Calc "
        "  public method M(b: B<int?>): int "
        "    var a := (case b of "
        "      var r: R => 1 "  // L exempt: neither pre-image satisfies IShape
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(exempt));       // L filtered out
    BEZEL_EXPECT_FALSE(exempt.diagnostics.hasErrors());
}

// --- s10: composed-trichotomy validation (spec §19.6 v0.196; #1816 s1–s8 all
//     landed) -----------------------------------------------------------------
//
// s1–s8 each pin ONE determination mechanism in isolation (ground mismatch,
// parameter solving, free-parameter undetermination, rigid identity, scalar
// `where`, type-bound `where`, the `?`-split). §19.6 v0.196's soundness
// argument (design doc §3, critique amendment A3) is explicit that the
// completeness proof holds only "when every leaf is determined or exempt" —
// i.e. covering determined leaves and ignoring exempt ones is complete ONLY as
// long as no leaf is left undetermined. These tests confirm the trichotomy
// COMPOSES: several mechanisms firing together inside one hierarchy at one
// match site must still yield the same per-leaf verdicts as they do in
// isolation, and a single undetermined leaf must still force `_` even when
// every other leaf in the same hierarchy is cleanly determined-and-covered or
// exempt.

// (a) THE s10 composed delta. `Result<T>` mixes THREE mechanisms in one
// hierarchy: `Ok<T>: Result<T>` is DETERMINED (s2, `T := int`) and its arm is
// given; `IntOnly: Result<string>` is EXEMPT (s1, ground mismatch against the
// `Result<int>` scrutinee) and needs no arm; `Tagged<U>: Result<int>` has U
// with NO occurrence in its (already-ground) root instantiation, so U is free
// after solving (s3) — infinitely many inhabiting instantiations — and is
// UNDETERMINED however the arm is typed. Per the design doc's A3 fix, the
// determined-and-exempt leaves being fully resolved does NOT rescue the
// hierarchy: coverage is statically unattainable while Tagged remains
// undetermined, so the case is non-exhaustive without `_` even though `Ok` is
// covered and `IntOnly` needs nothing.
BEZEL_TEST(sema_case_exhaust,
           mixed_determined_exempt_undetermined_still_needs_discard_spec_19_6) {
    // §19.6 v0.196 "Generic leaves: the determination rule", design doc A3 —
    // completeness holds only when EVERY leaf is determined or exempt; one
    // undetermined leaf (Tagged) still forces `_` despite Ok determined+covered
    // and IntOnly exempt.
    auto warned = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "               // determined: T:=int
        "sealed class IntOnly : Result<string> end IntOnly; "   // exempt: ground mismatch
        "sealed class Tagged<U> : Result<int> end Tagged; "     // U free -> undetermined
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "        // Ok determined and covered
        "      var g: Tagged<int> => 2 "     // one instantiation — does NOT cover Tagged
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(warned));       // Tagged unattainable
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "Tagged"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "undetermined"));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(warned, "U"));
    BEZEL_EXPECT_FALSE(warned.diagnostics.hasErrors());

    // Same hierarchy WITH a `_` catch-all — the only remedy for the
    // undetermined leaf (§19.6); Ok's determined coverage and IntOnly's
    // exemption were never the problem, so adding `_` alone clears it.
    auto cleared = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class IntOnly : Result<string> end IntOnly; "
        "sealed class Tagged<U> : Result<int> end Tagged; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "
        "      var g: Tagged<int> => 2, "
        "      _ => 0); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(cleared));      // `_` completes it
    BEZEL_EXPECT_FALSE(cleared.diagnostics.hasErrors());
}

// (b) The canonical DETERMINED idiom — "every leaf parameter mentioned in the
// base clause" — stays exhaustive with NO `_`, composed across TWO
// determination mechanisms landing on the same verdict in one hierarchy: `Ok`
// and `Err` are determined by direct parameter solving (s2, `T := int?`), and
// `Nullable<Y>: Result<Y?> where Y: notnull` is determined by the §7.10
// `?`-split collapsing to a singleton under the `notnull` bound (s7: the
// nullable pre-image `Y := int?` is excluded, leaving only `Y := int`). All
// three leaves are determined and covered, so the three-arm case is exhaustive
// without `_` — the s2+s7 composition must not regress the accept-delta either
// mechanism pins alone.
BEZEL_TEST(sema_case_exhaust,
           determined_idiom_composed_with_qmark_split_stays_exhaustive_spec_19_6) {
    // §19.6 v0.196 + §7.10 v0.159 — s2 (direct parameter solving) and s7
    // (`?`-idempotence pre-image split, constraint-collapsed to determined)
    // composed in one hierarchy: every leaf determined-and-covered, no `_`.
    auto r = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "               // T:=int? — determined
        "sealed class Err<T> : Result<T> end Err; "             // T:=int? — determined
        "sealed class Nullable<Y> : Result<Y?> where Y: notnull end Nullable; "
        "class Calc "
        "  public method M(r: Result<int?>): int "
        "    var a := (case r of "
        "      var o: Ok<int?> => 1, "      // T:=int? — the sole solution
        "      var e: Err<int?> => 2, "     // T:=int? — the sole solution
        "      var n: Nullable<int> => 3 "  // Y:=int — the sole surviving pre-image
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // all three determined — no `_`
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) A mix of DETERMINED+covered and EXEMPT leaves — with NO undetermined
// leaf in the hierarchy — is exhaustive with NO `_`: `Ok`/`Err` are determined
// by direct parameter solving (s2) and covered, while `Shapely<Y>: Result<Y>
// where Y: IShape` is EXEMPT because the solved `Y := int` provably fails the
// type-bound (s6: `int` is not a subtype of the user interface `IShape`, so
// the sole clause-(a) solution fails clause (b) and the set empties). Unlike
// (a) above, no leaf here is left undetermined, so the completeness argument
// (design doc §3) applies cleanly: covering the determined leaves and ignoring
// the exempt one is a complete proof.
BEZEL_TEST(sema_case_exhaust,
           determined_and_exempt_mix_no_undetermined_stays_exhaustive_spec_19_6) {
    // §19.6 v0.196 — s2 (determined) composed with s6 (type-bound exempt), no
    // undetermined leaf present: exhaustive without `_`.
    auto r = parseSemaAll(
        "module M; "
        "interface IShape end IShape; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "               // determined: T:=int
        "sealed class Err<T> : Result<T> end Err; "             // determined: T:=int
        "sealed class Shapely<Y> : Result<Y> where Y: IShape end Shapely; "  // exempt
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "
        "      var e: Err<int> => 2 "  // Shapely exempt — int is not IShape
        "); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));  // Shapely exempt, rest covered
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- s11: cross-module determination stability (spec §19.6 v0.196, the
//     cross-module subsection, line ~3897/~3902; design doc A16) -------------
//
// §19.6's cross-module subsection: "the closed leaf set is computed from the
// declaring module's source, and determination is evaluated at each match
// site against that set and the site's scrutinee type — an upstream-added
// leaf enters a recompiled match as determined, undetermined, or exempt by
// the same computation." The checker implements this by walking
// `ctx_.symbols()` for descendants of the scrutinee's root at EACH call to
// `checkCaseExhaustiveness` (src/sema/ExprTypecheck.cpp) — i.e. the closed set
// is recomputed fresh per compilation from the declaring module's own
// declarations, never cached or keyed off which other modules happen to be
// imported at the match site. The invariant under test: the verdict for a
// given leaf depends ONLY on the declaring module's closed set and the site's
// scrutinee type, never on ambient visibility (what else is declared/visible
// in the compilation) or declaration order.
//
// LIMITATION: `parseSemaAll` drives the single-file sema-unit harness (one
// `module` declaration, no `import`/multi-artifact linking) — it cannot
// exercise a genuine two-module compile+import (that is `tests/module/*_test.cpp`,
// which drives the full Driver). These tests approximate the cross-module
// story at the closed-set-from-declaring-symbols granularity the task
// describes: each independent `parseSemaAll` call is a fresh, from-scratch
// recomputation (mirroring "recompiled downstream match" against a changed
// declaring-module source), and the census-prohibition-style probe in (b)
// pins that the computation reads only descendants of the matched root, never
// unrelated ambient symbols or their declaration order.

// (a) Upstream leaf addition — recompiling a downstream match against a
// declaring-module source that has gained a new sealed leaf. BEFORE: `Ok<T>`
// and `Err<T>` are the hierarchy's only leaves; the two-arm match at
// `Result<int>` is exhaustive (both determined and covered). AFTER: the same
// declaring module gains `Pending<T>: Result<T>` (a new sealed leaf, as if an
// upstream recompile added one); the SAME match site (same two arms, no `_`)
// is now non-exhaustive, because the SAME determination computation applies
// to `Pending` too — `T := int` is its sole solution, so it is determined and
// (being uncovered) draws the warning naming it. §19.6: "an upstream-added
// leaf enters a recompiled match as determined, undetermined, or exempt by
// the same computation" — here Pending enters as determined.
BEZEL_TEST(sema_case_exhaust,
           cross_module_upstream_leaf_addition_recomputes_same_site_spec_19_6) {
    // BEFORE the upstream addition: Ok/Err alone are exhaustive at Result<int>.
    auto before = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "
        "      var e: Err<int> => 2); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(before));
    BEZEL_EXPECT_FALSE(before.diagnostics.hasErrors());

    // AFTER: the declaring module gained `Pending<T>: Result<T>`; the SAME
    // match site (same arms, still no `_`) recomputes non-exhaustive, and
    // names the newly-determined, newly-uncovered leaf.
    auto after = parseSemaAll(
        "module M; "
        "closed class Result<T> end Result; "
        "sealed class Ok<T> : Result<T> end Ok; "
        "sealed class Err<T> : Result<T> end Err; "
        "sealed class Pending<T> : Result<T> end Pending; "  // upstream addition
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "
        "      var e: Err<int> => 2); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(after));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(after, "Pending"));
    BEZEL_EXPECT_FALSE(after.diagnostics.hasErrors());
}

// (b) Ambient-visibility / declaration-order independence — the census
// prohibition (s6, design doc B2) generalized: the SAME hierarchy and SAME
// match site as (a)'s AFTER snapshot, but with unrelated declarations (an
// unrelated class, an unrelated interface, a wholly separate closed
// hierarchy) interleaved BEFORE, BETWEEN, and AFTER the `Result` family's own
// declarations, and with the leaves declared out of the "natural" order
// (`Pending` first, `Ok` last). The verdict must be byte-for-byte identical to
// (a)'s AFTER: the warning still fires and still names exactly `Pending` — a
// change to what else is visible in the compilation, or to the order leaves
// are declared in, must never change a leaf's determination. This pins that
// the closed-set walk (`ctx_.symbols()` filtered by descends-from-`sub`) reads
// structure, not a happens-to-be-visible census.
BEZEL_TEST(sema_case_exhaust,
           cross_module_verdict_independent_of_ambient_visibility_spec_19_6) {
    // §19.6 v0.196 cross-module subsection + design doc A16/B2 — ambient
    // unrelated declarations and leaf declaration order must not perturb the
    // determination verdict from (a)'s AFTER snapshot.
    auto r = parseSemaAll(
        "module M; "
        "class Unrelated1 end Unrelated1; "
        "interface IUnrelated end IUnrelated; "
        "closed class OtherHierarchy end OtherHierarchy; "
        "sealed class OtherLeaf : OtherHierarchy end OtherLeaf; "
        "closed class Result<T> end Result; "
        "sealed class Pending<T> : Result<T> end Pending; "  // declared FIRST this time
        "sealed class Err<T> : Result<T> end Err; "
        "class Padding end Padding; "                        // more ambient noise
        "sealed class Ok<T> : Result<T> end Ok; "             // declared LAST this time
        "class Calc "
        "  public method M(r: Result<int>): int "
        "    var a := (case r of "
        "      var o: Ok<int> => 1, "
        "      var e: Err<int> => 2); "
        "    return a; "
        "  end M; "
        "end Calc;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "Pending"));
    // The ambient/reordered declarations must not themselves be misreported
    // as part of the Result closed set.
    BEZEL_EXPECT_FALSE(nonExhaustiveWarningMentions(r, "OtherLeaf"));
    BEZEL_EXPECT_FALSE(nonExhaustiveWarningMentions(r, "Unrelated1"));
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
        "    var n := (case a of "
        "      Dog => 1, "
        "      Cat => 2 "  // open base: more subclasses could exist
        "); "
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
        "    var n := (case a of "
        "      Dog => 1, "
        "      Cat => 2, "
        "      _ => 0); "
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
        "    var n := (case score of "
        "      90..100 => 1, "
        "      80..89 => 2 "  // no `_`: rest of the int range uncovered
        "); "
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
        "    var grade := (case score of "
        "      90..100 => \"A\", "
        "      80..89 => \"B\", "
        "      70..<80 => \"C\", "
        "      _ => \"F\"); "
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
        "    return (case x of null => 0, var n: int => n); "
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
        "    return (case x of var n: int => n); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
}

// A typed binding over int? WITHOUT a `null` arm is still non-exhaustive.
BEZEL_TEST(sema_case_exhaust, nullable_typed_binding_no_null_arm_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(x: int?): int "
        "    return (case x of var n: int => n); "
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
        "  return (case a of var d: Dog => 1); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
}

// --- Record scrutinee (§19.6 "Record scrutinee" bullet, ruling #4372, fixed
// by #4674) -------------------------------------------------------------
//
// Through fix #4674, `caseArmsExhaustive` had NO branch at all for a Record
// scrutinee: an all-slots-irrefutable positional destructure fell through to
// the generic "cannot prove exhaustive" catch-all and drew a spurious
// warning, even though §19.6 states a record has exactly one shape and is
// implicitly sealed (§9.6), so covering that shape covers every value and no
// `_` arm is required. These tests pin THE BUG (#4674's own repro) and its
// companion (a refutable slot must still warn, so the fix is not a blanket
// suppression — the generic catch-all only happened to agree with the spec
// there by accident, per #4674's own diagnosis).

// #4674's own repro: `Point(x, y) => x + y` — a positional destructure whose
// two naked-binding slots are both irrefutable — must be silently exhaustive.
BEZEL_TEST(sema_case_exhaust, record_positional_naked_bindings_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point): int "
        "    return (case p of "
        "      Point(x, y) => x + y); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same shape with explicit `var` bindings (`Point(var x, var y)`) — an
// untyped `var` binding is irrefutable exactly as a naked binding is
// (§19.6: "every of whose slots is irrefutable (a binding or `_`)").
// Corpus twin: s19_6_0010 / s19_6_0031.
BEZEL_TEST(sema_case_exhaust, record_positional_var_bindings_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point): int "
        "    return (case p of "
        "      Point(var x, var y) => x + y); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `_` discard slot is irrefutable too — `Point(_, y)` still covers every
// value of `Point`.
BEZEL_TEST(sema_case_exhaust, record_positional_discard_slot_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point): int "
        "    return (case p of "
        "      Point(_, y) => y); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// COMPANION (the fix must not become a blanket suppression): a REFUTABLE
// slot — a constant `0` — makes the arm contribute nothing, exactly as a
// refutable choice-case slot does (§19.6: "Coverage is one level deep with
// no per-slot value-space analysis"). Corpus twin: s19_6_0011 / s19_6_0033
// (blocked on the separate #4626 diagnostic-wording issue, not this fix).
BEZEL_TEST(sema_case_exhaust, record_positional_refutable_slot_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method YOf(p: Point): int "
        "    return (case p of "
        "      Point(0, var y) => y); "
        "  end YOf; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An exact-type pattern (`var q: Point`) over a non-nullable record scrutinee
// is itself total (§19.3: over a non-nullable value scrutinee a type pattern
// naming its exact type always matches) — this path is handled by the
// pre-existing typed-binding check (it does not gate on the scrutinee's
// TypeDeclKind), so this test pins that the new Record branch does not
// disturb it. Corpus twin: s19_6_0032.
BEZEL_TEST(sema_case_exhaust, record_exact_type_pattern_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public method FirstOf(p: Point): int "
        "    return (case p of "
        "      var q: Point => q.X); "
        "  end FirstOf; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A DESTRUCTURE whose head names a DIFFERENT record than the scrutinee does
// NOT cover it — records have no subtyping (§9.6), so a differently-headed
// destructure must not falsely satisfy exhaustiveness.
BEZEL_TEST(sema_case_exhaust,
           record_positional_wrong_type_head_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "record Other(A: int, B: int); "
        "class C "
        "  public static method Sum(p: Point): int "
        "    return (case p of "
        "      Other(a, b) => a); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
}

// --- Nullable record scrutinee (§19.6: "the rules above plus an explicit
// `null` pattern") --------------------------------------------------------

// An all-slots-irrefutable destructure plus an explicit `null` arm covers a
// nullable record scrutinee.
BEZEL_TEST(sema_case_exhaust, nullable_record_with_null_arm_ok_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point?): int "
        "    return (case p of "
        "      Point(x, y) => x + y, "
        "      null => 0); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same destructure WITHOUT a `null` arm leaves a nullable record
// scrutinee non-exhaustive — the destructure covers only the non-null
// values.
BEZEL_TEST(sema_case_exhaust,
           nullable_record_no_null_arm_warns_spec_19_6) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point?): int "
        "    return (case p of "
        "      Point(x, y) => x + y); "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- §21.7 statement-`case` totality grant over a Record scrutinee
// (fix #4685, follows #4674) ------------------------------------------
//
// `stmtAlwaysExits`'s `admitsTotality` gate (a DIFFERENT function from
// `caseArmsExhaustive`, reusing its verdict) hardcoded Bool/Choice/Enum/
// Class and had no Record arm, so even after #4674 taught
// `caseArmsExhaustive` to prove a record `case` exhaustive, a STATEMENT-
// position `case` over that same exhaustive record destructure — every
// arm diverging — still fell through to "does not return a value on every
// path (§21.7)" instead of being granted totality. The #4372 ruling states
// the registry directly: "§21.7's statement-`case` totality grant is keyed
// to exactly this property, and this list is its registry."

// The #4685 repro: an all-slots-irrefutable positional record destructure,
// its only arm a `return`, as the WHOLE body of a value-returning method.
// Before the fix this drew the §21.7 "does not return a value on every
// path" error even though the case only has one reachable outcome.
BEZEL_TEST(sema_case_exhaust,
           record_positional_stmt_case_totality_granted_spec_21_7) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point): int "
        "    case p of "
        "      Point(x, y) => return x + y; "
        "    end; "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReturnEveryPathError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// COMPANION (must not become a blanket totality grant for every record
// `case`): a REFUTABLE slot leaves the record `case` non-exhaustive
// (§19.6), so §21.7 totality must still be WITHHELD — the method falls
// through with no return on the excluded value and the error must fire.
BEZEL_TEST(sema_case_exhaust,
           record_positional_refutable_stmt_case_totality_withheld_spec_21_7) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method YOf(p: Point): int "
        "    case p of "
        "      Point(0, var y) => return y; "
        "    end; "
        "  end YOf; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReturnEveryPathError(r));
}

// A NULLABLE record scrutinee: the destructure plus an explicit `null` arm,
// both diverging, grants totality (§19.6's nullable rule: "the rules above
// plus an explicit `null` pattern").
BEZEL_TEST(sema_case_exhaust,
           nullable_record_stmt_case_totality_granted_spec_21_7) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point?): int "
        "    case p of "
        "      Point(x, y) => return x + y;, "
        "      null => return 0; "
        "    end; "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasReturnEveryPathError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// COMPANION: the same nullable destructure WITHOUT the `null` arm leaves
// the case non-exhaustive (only the non-null values are covered), so
// totality must still be withheld.
BEZEL_TEST(sema_case_exhaust,
           nullable_record_no_null_arm_stmt_case_totality_withheld_spec_21_7) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Sum(p: Point?): int "
        "    case p of "
        "      Point(x, y) => return x + y; "
        "    end; "
        "  end Sum; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasReturnEveryPathError(r));
}
