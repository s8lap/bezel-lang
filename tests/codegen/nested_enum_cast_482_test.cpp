// Codegen tests for #482: `int(expr)` on a NESTED enum type crashes with an
// LLVM assertion `cast<Ty>() argument of incompatible type!`.
//
// A nested enum (`class Outer public enum Inner ...; end Inner; end Outer;`)
// differs from a module-level enum in three ways that the pre-fix compiler
// failed to handle:
//   A. computeEnumValues() never visited nested enums, so no `enumValue_`
//      entry existed for their constants → lookups returned 0 / null.
//   B. The sema and codegen enum-cast paths only recognised `TypeSymbol*`
//      directly; a `NestedTypeSymbol` wraps the inner `TypeSymbol` in its
//      `->type` and does NOT inherit `TypeSymbol`, so the dynamic_cast missed
//      it and the call was left without an enum-cast type / lowering.
//   C. TypeResolution stored nested-type names in `typeScope.names` as `nullptr`,
//      colliding with the generic-param sentinel; a single-part reference to
//      a nested type name inside the outer class body would be mislabelled as
//      a generic param.
//
// Spec references: §10 (enums, underlying integer), §5.2 / §17 (explicit
// conversion `int(e)` yields the underlying value, §17 conversion expressions).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Helper: build a module with Container (which has a nested Status enum)
// and a Main2 class with the given method body.
int run482(const std::string& body) {
    auto r = runJit(
        "module M; "
        "class Container "
        "  public enum Status Active, Inactive; end Status; "
        "  public _status: Container.Status := Container.Status.Active; "
        "  destructor end destructor; "
        "end Container; "
        "class Main2 "
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end Main2;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// (a) §10 / §17: `int(c._status)` on a nested-enum FIELD returns the
// underlying value — 0 for `Active` (the first constant, auto-assigned 0).
// This is the primary #482 repro: pre-fix the compiler hit an LLVM assertion
// `cast<Ty>() argument of incompatible type!` in lowerConversion.
BEZEL_TEST(codegen_482_spec_10_spec_17, field_read_first_constant_is_zero) {
    BEZEL_EXPECT_EQ(run482(
        "using var c := new Container(); "
        "return int(c._status);"), 0);
}

// (a2) §10 / §17: assigning the SECOND constant to the field and then
// casting it must return 1 (auto-increment from 0).
BEZEL_TEST(codegen_482_spec_10_spec_17, field_read_second_constant_is_one) {
    BEZEL_EXPECT_EQ(run482(
        "using var c := new Container(); "
        "c._status := Container.Status.Inactive; "
        "return int(c._status);"), 1);
}

// (b) §10 / §17: `int(Container.Status.Inactive)` — cast of a CONSTANT
// expression (not a field read) for the nested enum. Pre-fix, computeEnumValues
// never populated `enumValue_` for nested-enum constants, so `enumConstant()`
// returned null and the constant lowered to 0 regardless.
BEZEL_TEST(codegen_482_spec_10_spec_17, constant_expr_inactive_is_one) {
    BEZEL_EXPECT_EQ(run482(
        "return int(Container.Status.Inactive);"), 1);
}

// (b2) §10 / §17: the first constant Active == 0.
BEZEL_TEST(codegen_482_spec_10_spec_17, constant_expr_active_is_zero) {
    BEZEL_EXPECT_EQ(run482(
        "return int(Container.Status.Active);"), 0);
}

// (c) §10 / §17: nested-enum local variable — assign from a constant,
// read back, cast to int.
BEZEL_TEST(codegen_482_spec_10_spec_17, local_variable_cast) {
    BEZEL_EXPECT_EQ(run482(
        "var s := Container.Status.Inactive; "
        "return int(s);"), 1);
}

// (d) §10 / §19: nested enum used in an equality comparison still works.
BEZEL_TEST(codegen_482_spec_10_spec_17, equality_comparison) {
    BEZEL_EXPECT_EQ(run482(
        "using var c := new Container(); "
        "return (7 if c._status = Container.Status.Active else 0);"), 7);
}

// (d2) §10 / §19: nested enum used in a `case` arm.
BEZEL_TEST(codegen_482_spec_10_spec_17, case_expr_on_nested_enum) {
    BEZEL_EXPECT_EQ(run482(
        "var s := Container.Status.Inactive; "
        "return case s of "
        "  Container.Status.Active   => 0, "
        "  Container.Status.Inactive => 9, "
        "  _ => -1, "
        "end;"), 9);
}

// (e) CONTROL: a module-level enum `int()` cast must still work (regression
// guard — the fix must not perturb the top-level enum path).
BEZEL_TEST(codegen_482_spec_10_spec_17, toplevel_enum_int_cast_control) {
    BEZEL_EXPECT_EQ(runJit(
        "module M; "
        "enum Direction North, South, East, West; end Direction; "
        "class C "
        "  public static method Main(): int "
        "    return int(Direction.East); "
        "  end Main; "
        "end C;").exitCode, 2);
}

// (e2) CONTROL: a module-level enum from-int cast (E(i)) still works.
BEZEL_TEST(codegen_482_spec_10_spec_17, toplevel_enum_from_int_cast_control) {
    BEZEL_EXPECT_EQ(runJit(
        "module M; "
        "enum Direction North, South, East, West; end Direction; "
        "class C "
        "  public static method Main(): int "
        "    var d := Direction(1); return int(d); "
        "  end Main; "
        "end C;").exitCode, 1);
}

// (f) §10: nested enum with explicit integer values.
BEZEL_TEST(codegen_482_spec_10_spec_17, nested_enum_explicit_values) {
    auto r = runJit(
        "module M; "
        "class Container2 "
        "  public enum Code Ok := 200, NotFound := 404; end Code; "
        "  destructor end destructor; "
        "end Container2; "
        "class Main3 "
        "  public static method Main(): int "
        "    return int(Container2.Code.NotFound) - 400; "
        "  end Main; "
        "end Main3;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 4);
}
