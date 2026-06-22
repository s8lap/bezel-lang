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

// ===========================================================================
// §9.5 (#933): `with` on an EXISTING class instance is the object-initializer
// form (§22.11) and is valid only on a freshly-`new`-constructed instance.
// Applied to an existing class variable it would silently mutate/alias it.
// ===========================================================================

// REJECT: `with` on an existing class variable.
BEZEL_TEST(with_expression_target_validation_875, with_on_existing_class_rejected_933) {
    auto r = parseSemaAll(
        "module M; "
        "class Box "
        "  public property V: int; "
        "  public constructor() V := 0; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b1 := new Box(); b1.V := 7; "
        "  var b2 := b1 with V := 42 end; "  // existing class instance — rejected
        "  return b2.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ACCEPT: the object-initializer form on a `new` class expression.
BEZEL_TEST(with_expression_target_validation_875, with_on_new_class_accepted_933) {
    auto r = parseSemaAll(
        "module M; "
        "class Box "
        "  public property V: int; "
        "  public constructor() V := 0; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box() with V := 42 end; "  // new source — accepted
        "  return b.V; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: `with` on an existing RECORD variable (value-type copy — unaffected).
BEZEL_TEST(with_expression_target_validation_875, with_on_existing_record_accepted_933) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var p2 := p with X := 5 end; "  // record copy — accepted
        "  return p2.X; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §9.5 (#935): a field name may appear at most once in a `with` initializer
// list (the names are static identifiers, so a duplicate is always detectable
// — mirrors §15.4 duplicate-Dictionary-key). The first assignment is dead.
// ===========================================================================

// REJECT: duplicate field name in a record `with`.
BEZEL_TEST(with_expression_target_validation_875, duplicate_field_in_record_with_rejected_935) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var p2 := p with X := 5, X := 10 end; "  // X twice — rejected
        "  return p2.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("assigned more than once") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// REJECT: duplicate field name in a class object-initializer `with`.
BEZEL_TEST(with_expression_target_validation_875, duplicate_field_in_class_with_rejected_935) {
    auto r = parseSemaAll(
        "module M; "
        "class Box "
        "  public property V: int; "
        "  public constructor() V := 0; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box() with V := 3, V := 4 end; "  // V twice
        "  return b.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ACCEPT: distinct field names (record copy + class object-initializer).
BEZEL_TEST(with_expression_target_validation_875, distinct_fields_in_with_accepted_935) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var p2 := p with X := 5, Y := 9 end; "
        "  return p2.X + p2.Y; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
