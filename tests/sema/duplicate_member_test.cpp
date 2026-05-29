// Spec-derived tests for Sema slice S14 — duplicate member names
// within a type (§11.3 lists member kinds; uniqueness is a well-
// formedness requirement). Methods/constructors may overload; all other
// member kinds must have unique names and cannot collide with anything.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_dup, duplicate_fields_diagnose) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: int; "
        "  public _x: int; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_dup, field_and_method_same_name_diagnose) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public Value: int; "
        "  public method Value() _ := 1; end Value; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_dup, method_overloads_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Add(a: int) _ := 1; end Add; "
        "  public method Add(a: int, b: int) _ := 2; end Add; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_dup, distinct_field_names_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _x: int; "
        "  public _y: int; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_dup, duplicate_in_nested_type_diagnose) {
    auto r = parseSemaAll(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public _z: int; "
        "    public _z: int; "
        "  end Inner; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
