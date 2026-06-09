// Spec-derived tests for §27.8 — type-alias transparency (issue #578).
//
// §27.8: A type alias is a pure synonym for its underlying type.  An alias-typed
// value must be accepted everywhere the underlying type is accepted: in operator
// dispatch, lambda-target-type inference, assignability, and parameter passing.
//
// Two families of defects are covered:
//
//   6a — operator dispatch on alias-typed values.
//       `type Meters = double; var x: Meters := 5.0; var sum := x + y;`
//       must NOT produce an "operator '+' is not defined" error — the alias
//       unfolds to `double` before operator lookup.
//
//   6b — lambda assignment to a function-type alias variable.
//       `type Pred = (int) -> bool; var f: Pred := (x) => x mod 2 = 0;`
//       must NOT produce an "initializer not implicitly convertible" error —
//       the alias unfolds to its Function type before the assignability check.
//
// Control cases ensure the fix does not regress:
//   - arithmetic on the underlying type still works (no regression)
//   - parameter passing with an alias-typed arg still works (already worked)
//   - an alias of a user class type still works for member lookup
//   - the error for a true user-type without an operator is still reported

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::LocalSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

LocalSymbol* findLocal(const bezel::test::SemaResult& r,
                       std::string_view name) {
    for (const auto& s : r.ctx->symbols()) {
        if (auto* ls = dynamic_cast<LocalSymbol*>(s.get()))
            if (ls->name == name) return ls;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// 6a §27.8 — primitive alias operators: Meters + Meters must not be an error.
// Before fix: "operator '+' is not defined for type '?'".
// After fix: no error; the alias unfolds to double before operator lookup.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_alias_transparency_578,
           alias_of_double_plus_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Meters = double; "
        "class C public static method M(a: Meters, b: Meters): Meters "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// 6a §27.8 — Meters + Meters result is typed as double (the alias's underlying
// type). The inferred result local must carry a Primitive/Double declType.
BEZEL_TEST(sema_alias_transparency_578,
           alias_plus_result_typed_as_underlying_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Meters = double; "
        "class C public method M() "
        "  var a: Meters := 5.0; "
        "  var b: Meters := 3.0; "
        "  var sum := a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* sum = findLocal(r, "sum");
    BEZEL_EXPECT_TRUE(sum != nullptr);
    if (sum && sum->declType) {
        BEZEL_EXPECT_TRUE(sum->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(sum->declType->primitive == PrimitiveType::Double);
    }
}

// 6a §27.8 — alias of int: arithmetic operators (+, -, *, /) work transparently.
BEZEL_TEST(sema_alias_transparency_578,
           alias_of_int_arithmetic_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Count = int; "
        "class C public static method M(a: Count, b: Count): int "
        "  var s := a + b; "
        "  var d := a - b; "
        "  var p := a * b; "
        "  var q := a / b; "
        "  return s; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// 6a §27.8 — Meters + double (mixed: alias + underlying type). §5.1 numeric
// widening: both sides are double, so no widening needed.
BEZEL_TEST(sema_alias_transparency_578,
           alias_plus_underlying_mixed_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Meters = double; "
        "class C public static method M(a: Meters, b: double): double "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 6b §27.8 — function-type alias lambda assignment must not be an error.
// Before fix: "initializer not implicitly convertible to the target type".
// After fix: no error; the alias unfolds to (int)->bool before the check.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_alias_transparency_578,
           function_type_alias_lambda_assign_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Pred = (int) -> bool; "
        "class C public method M() "
        "  var isEven: Pred := (x) => x mod 2 = 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// 6b §27.8 — a Pred local carries a Function declType after the fix.
BEZEL_TEST(sema_alias_transparency_578,
           function_type_alias_local_has_function_type_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Pred = (int) -> bool; "
        "class C public method M() "
        "  var isEven: Pred := (x) => x mod 2 = 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* f = findLocal(r, "isEven");
    BEZEL_EXPECT_TRUE(f != nullptr);
    if (f && f->declType) {
        BEZEL_EXPECT_TRUE(f->declType->kind == TypeRef::Kind::Function);
    }
}

// 6b §27.8 — alias of a two-parameter function type.
BEZEL_TEST(sema_alias_transparency_578,
           function_type_alias_two_params_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Combiner = (int, int) -> int; "
        "class C public method M() "
        "  var add: Combiner := (a, b) => a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Control: parameter passing with alias-typed argument already worked (regression).
// §27.8: an alias is transparent in parameter-passing position.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_alias_transparency_578,
           alias_arg_to_underlying_param_no_error_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "type Meters = double; "
        "class C "
        "  public static method Use(x: double): double => x; "
        "  public static method M(m: Meters): double => Use(m); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: alias of a user class type — member lookup must still work.
BEZEL_TEST(sema_alias_transparency_578,
           alias_of_class_member_lookup_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "class Point public X: int := 0; end Point; "
        "type Pt = Point; "
        "class C public static method M(p: Pt): int => p.X; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: the error for a true user-defined type with NO operator is still
// reported. The fix must NOT suppress legitimate §25 errors.
BEZEL_TEST(sema_alias_transparency_578,
           alias_of_class_without_operator_still_errors_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "class Kg public V: double := 0.0; end Kg; "
        "type Weight = Kg; "
        "class C public static method M(a: Weight, b: Weight): double "
        "  var s := a + b; "
        "  return 0.0; "
        "end M; end C;");
    // Kg has no `operator +`, so this is still a §25 compile error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "+"));
}

// Control: direct underlying-type use still works (baseline correctness).
BEZEL_TEST(sema_alias_transparency_578,
           underlying_double_plus_no_error_regression_spec_27_8) {
    auto r = parseSema(
        "module M; "
        "class C public static method M(a: double, b: double): double "
        "  return a + b; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
