// Spec-derived tests for the `with`-expression target-validation fix
// (§9.5/§5.1, fix #875 #876 #877 #878).
//
// §9.5: "The named fields must be auto-generated positional-field properties,
// auto-properties declared in the record body, or public fields."
// A nonexistent field, a method, or a computed property is not a valid `with`
// target. The replacement value must be implicitly convertible to the field
// type (§5.1).
//
// Tests:
//   REJECT: wrong-type replacement value (#875)
//   REJECT: nonexistent field name (#876)
//   REJECT: method name as target (#877)
//   REJECT: computed property as target (#878)
//   ACCEPT: valid positional-field replacement (common case)
//   ACCEPT: multiple-field replacement
//   ACCEPT: widening value (int → long)
//   ACCEPT: auto-property-with-backing-storage target

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

// Helper: does any diagnostic mention a §9.5 `with` target error?
bool hasWithTargetError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("9.5") != std::string::npos ||
            d.message.find("with") != std::string::npos)
            return true;
    }
    return false;
}

// Helper: does any diagnostic mention a §5.1 type-compatibility error?
bool hasTypeCompatError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("5.1") != std::string::npos ||
            d.message.find("not implicitly convertible") != std::string::npos ||
            d.message.find("replacement value") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §9.5/§5.1, #875: a wrong-type replacement value is a compile error.
// `p with X := "hi" end` where X: int must be rejected.
BEZEL_TEST(with_expression_target_validation_875, wrong_type_replacement_rejected) {
    auto r = parseSemaAll(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var q := p with X := \"hi\" end; "
        "  return q.Y; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasTypeCompatError(r));
}

// §9.5, #876: a nonexistent field name is a compile error.
// `p with Z := 99 end` where Point has no Z must be rejected.
BEZEL_TEST(with_expression_target_validation_875, nonexistent_field_rejected) {
    auto r = parseSemaAll(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 7); "
        "  var q := p with Z := 99 end; "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWithTargetError(r));
}

// §9.5, #877: a method name as a `with` target is a compile error.
// `p with Double := 99 end` where Double is a method must be rejected.
BEZEL_TEST(with_expression_target_validation_875, method_target_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public method Double(): int => X * 2 + Y; "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  var q := p with Double := 99 end; "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWithTargetError(r));
}

// §9.5, #878: a computed (expression-bodied) property is not a valid `with`
// target because it has no backing storage.
BEZEL_TEST(with_expression_target_validation_875, computed_property_target_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record ColorVal(R: int, G: int, B: int) "
        "  public property Luminance: int => (R * 3 + G * 6 + B) / 10; "
        "end ColorVal; "
        "class C public static method Main(): int "
        "  var c := new ColorVal(10, 20, 30); "
        "  var d := c with Luminance := 99 end; "
        "  return d.R; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWithTargetError(r));
}

// §9.5, #875-878 positive: valid positional-field replacement still compiles.
// `p with X := 5 end` where X: int and 5: int is accepted.
BEZEL_TEST(with_expression_target_validation_875, valid_positional_field_accepted) {
    auto r = parseSemaAll(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var q := p with X := 5 end; "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5, #875-878 positive: multiple-field replacement still compiles.
BEZEL_TEST(with_expression_target_validation_875, multiple_fields_accepted) {
    auto r = parseSemaAll(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var q := p with X := 5, Y := 6 end; "
        "  return q.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5/§5.1, #875-878 positive: int → long widening is accepted (mirrors
// ordinary variable-declaration widening).
BEZEL_TEST(with_expression_target_validation_875, widening_value_accepted) {
    auto r = parseSemaAll(
        "module M; record Big(N: long); "
        "class C public static method Main(): int "
        "  var b := new Big(10); "
        "  var c := b with N := 5 end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.5, #875-878 positive: an auto-property with backing storage is a valid
// `with` target.
BEZEL_TEST(with_expression_target_validation_875, auto_property_target_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record Person "
        "  public Name: string; "
        "  public Age: int; "
        "end Person; "
        "class C public static method Main(): int "
        "  var p := new Person(); "
        "  var q := p with Age := 31 end; "
        "  return q.Age; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
