// Spec-derived tests for §19.3 (v0.311, #5171; ruling #4451): a class,
// interface, `string`, or `Object` TYPE pattern, with or without binding,
// over a primitive, record, enum, choice, inline-array, or unit scrutinee
// (or the nullable form of any of them), is a compile error; so is a
// pattern naming a different value type. A choice's case patterns are never
// type patterns (§35.5).
//
// Background (#1936's predicate at src/sema/ExprTypecheck.cpp:~21851 /
// src/sema/ExprTypecheck_Exprs.cpp:~11727): the predicate covered only
// binding-form typed-binding patterns (`var x: T`), and only against a
// non-nullable primitive (not string/Object) or a Record/Enum scrutinee —
// omitting choice, inline-array, unit, and nullable-value scrutinees, and
// omitting the BARE form (a top-level NamedPattern, e.g. `case c of IShape`).
// The codegen hazard #1936 traced (#1936's ICmpInst assertion in `emitIsA`
// over a non-pointer value) is reachable today through every gap above:
// `case c of IShape` over a value-like choice, `case () of Object`, etc.
// The fix widens the predicate.
//
// These tests pin the fix's scope. They are expected to FAIL on
// origin/main (the predicate does not yet cover these cases) and PASS with
// the fix applied.

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

// True iff an ERROR diagnostic mentions the §19.3 (v0.311, #5188) value-
// scrutinee type-pattern reason. The wording is the same family as #1936
// (so a single phrase catches both), but #5188 broadens it to more
// scrutinee kinds (choice/unit/inline-array/nullable) and to the bare form.
bool hasValueScrutTypePatternError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "value-type scrutinee can never match a reference-typed") !=
                std::string::npos)
            return true;
    }
    return false;
}

// Shared preamble: a class, an interface, a record, an enum, an interface
// that a choice will implement, and a class `Some` whose name overlaps a
// choice case named `Some` (the "case pattern named like an in-scope
// class" probe from the issue body).
constexpr const char* kDecls =
    "module M;\n"
    "class C public method M(): int return 0; end M; end C;\n"
    "interface I method M(): int; end I;\n"
    "interface IShape method M(): int; end IShape;\n"
    "record Pt(x: int, y: int);\n"
    "enum Color Red, Green; end Color;\n"
    "choice Outcome Fetched(body: int), NotModified; end Outcome;\n"
    "class Some public method M(): int return 0; end M; end Some;\n";

std::string prog(const std::string& body) {
    return std::string(kDecls) + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// =========================================================================
// §19.3 v0.311 / #5188 — REJECTED: choice-scrutinee type patterns
// =========================================================================

// (1) The issue's exact probe: `case c of IShape` over a value-like choice
// implementing IShape. Without the fix the arm reaches codegen, and the
// emitIsA path ICmp's the non-pointer struct against a null pointer (the
// #1936 hazard), aborting the backend. With the fix the arm is rejected
// at admission.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (2) The binding-form sibling: `case c of var s: IShape`. The current
// predicate is also missing the choice scrutinee on the binding path.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_vs_binding_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of var s: IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (3) `Object` is a §19.3 reference-typed pattern kind the current
// predicate OMITS. Without the fix this admits a nonsense `case c of Object`
// over a value-like choice (and codegen would try to test a value against
// `Object` — same emitIsA hazard).
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_vs_bare_object_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of Object => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (4) `string` is another omitted pattern kind.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_vs_bare_string_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of string => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (5) A user class pattern over a value-like choice scrutinee — the
// codegen hazard is `ICmpInst` against a class type-test, but the arm
// would be unmatchable anyway: a value type is never a class.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_vs_bare_class_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of C => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// =========================================================================
// §19.3 v0.311 / #5188 — REJECTED: inline-array and unit scrutinees
// =========================================================================

// (6) Inline-array scrutinee. The current predicate's scrutinee-side check
// only mentions primitives and Record/Enum; inline arrays are missed
// entirely. Without the fix the arm reaches codegen, which never produced
// a sound `emitIsA` for `[N]T`.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           inline_array_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var a: [3]int := [1, 2, 3];\n"
        "    return (case a of IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (7) Inline-array scrutinee, binding-form sibling.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           inline_array_vs_binding_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var a: [3]int := [1, 2, 3];\n"
        "    return (case a of var s: IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (8) Unit scrutinee, bare Object pattern. The current predicate is
// missing both the unit scrutinee kind and the Object pattern kind.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           unit_vs_bare_object_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return (case () of Object => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (9) Unit scrutinee, bare interface pattern.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           unit_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return (case () of IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (10) Unit scrutinee, binding-form interface pattern.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           unit_vs_binding_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return (case () of var s: IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// =========================================================================
// §19.3 v0.311 / #5188 — REJECTED: nullable value scrutinees
// =========================================================================

// (11) Nullable record scrutinee. The current predicate gates on
// `!scrutTy->nullable`; a `Pt?` value is a sealed value type either way,
// and `case p? of IShape` is just as much a compile error as `case p of
// IShape` — same boxing requirement that pattern position forbids.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           nullable_record_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p: Pt? := new Pt(1, 2);\n"
        "    return (case p of IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (12) Nullable enum scrutinee, binding-form.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           nullable_enum_vs_binding_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c: Color? := Color.Red;\n"
        "    return (case c of var s: IShape => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (13) Nullable primitive scrutinee, Object pattern. The current predicate
// only excludes string/Object for non-nullable primitives; with nullable,
// even an int? scrutinee against Object must be rejected — a value type
// is never Object without a boxing conversion (and pattern position
// forbids one).
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           nullable_int_vs_bare_object_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var n: int? := 5;\n"
        "    return (case n of Object => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// =========================================================================
// §19.3 v0.311 / #5188 — REJECTED: bare-form patterns (the new check site)
// =========================================================================

// (14) Bare-form record scrutinee vs a class pattern. The existing #1936
// predicate catches the BINDING form (`var q: C`) for records, but does
// not reject the bare `C` form — `checkTopLevelNamedPattern` resolves it
// as a valid type test and lets it through. Codegen still mis-compiles.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           record_vs_bare_class_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return (case p of C => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (15) Bare-form enum scrutinee vs an interface pattern — same shape.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           enum_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Color.Red;\n"
        "    return (case c of I => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (16) Bare-form primitive (int) scrutinee vs a class pattern — the
// ORIGINAL #1203 entry the predicate already catches for the BINDING form,
// but the bare-form path through `checkTopLevelNamedPattern` was never
// guarded. (#5188 closes the gap.)
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           int_vs_bare_class_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return (case 5 of C => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// =========================================================================
// §19.3 v0.311 / #5188 — REJECTED: STATEMENT-form (CaseStmt, not CaseExpr)
// =========================================================================

// (17) Statement-form sibling of (1): a choice scrutinee vs a bare
// interface pattern, in the CaseStmt admission site. The fix must apply
// at BOTH CaseExpr (ExprTypecheck_Exprs.cpp) and CaseStmt
// (ExprTypecheck.cpp).
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           stmt_choice_vs_bare_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.Fetched(7);\n"
        "    var n := 0;\n"
        "    case c of IShape => n := 1, _ => n := 9 end;\n"
        "    return n;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// (18) Statement-form: nullable int? scrutinee vs a binding interface
// pattern.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           stmt_nullable_int_vs_binding_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var n: int? := 5;\n"
        "    var r := 0;\n"
        "    case n of var s: IShape => r := 1, _ => r := 9 end;\n"
        "    return r;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutTypePatternError(r));
}

// =========================================================================
// §35.5 / §19.3 — NOT a type pattern: choice case patterns stay admitted
// =========================================================================

// (19) Per §35.5: a payload-free case name `NotModified` is a CASE
// pattern, not a type test, even though `NotModified` could resolve
// against the symbol table as a type-like name. The check must NOT
// reject this arm — it's the standard match over the choice's cases.
// This is the issue body's "case pattern named like an in-scope class"
// probe (here: payload-free, no in-scope `NotModified` class).
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_payload_free_case_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Outcome.NotModified;\n"
        "    return (case c of NotModified => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (20) Per §35.5: a payload-carrying case pattern `Fetched(body)`
// destructures the case. Even though a CLASS named `Some` (a separate
// symbol) is in scope, the choice case `Some(...)` MUST resolve as the
// case pattern, not the type test (`class Some`). The probe from the
// issue body: `Some(var x)` beside a class `Some` is still the case
// pattern.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           choice_case_pattern_named_like_in_scope_class_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "interface IShape method M(): int; end IShape;\n"
        "choice Outcome Some(v: int), None; end Outcome;\n"
        "class Some public method M(): int return 0; end M; end Some;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var o := Outcome.Some(7);\n"
        "    return (case o of Some(var x) => x, _ => 9);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// No over-rejection: legitimate reference-type tests stay ACCEPTED
// =========================================================================

// (21) The same pattern on a CLASS scrutinee (legitimate downcast) is
// unaffected. A `case a of var d: Dog` over `Animal` is the §19.3
// reference-type test path.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           class_scrutinee_subclass_pattern_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Animal public method Speak(): int return 0; end Speak; "
        "end Animal;\n"
        "class Dog : Animal public method Bark(): int return 1; end Bark; "
        "end Dog;\n"
        "class Program\n"
        "  public static method Take(a: Animal): int\n"
        "    return (case a of var d: Dog => 1, _ => 9);\n"
        "  end Take;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (22) Same-type value pattern: `case 5 of int` is admitted (§19.3: "a
// pattern naming its exact type always matches"). The widened predicate
// must NOT reject the same-type case.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           int_vs_int_same_type_pattern_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return (case 5 of int => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (23) Record destructuring over a record scrutinee — `Pt(a, b)` is the
// record positional-destructure form, NOT a type pattern. The check must
// leave it alone.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           record_destructure_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return (case p of Pt(a, b) => a + b, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (24) Object scrutinee (REFERENCE type) vs a class pattern — the
// legitimate runtime test path. The widened predicate is gated on a
// VALUE-shaped scrutinee, so a reference scrutinee is unaffected.
BEZEL_TEST(case_type_test_value_scrutinee_type_pattern_5188,
           object_scrutinee_class_pattern_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(o: Object): int\n"
        "    return (case o of var x: C => 1, _ => 9);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutTypePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
