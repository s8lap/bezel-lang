// Spec-derived sema tests for fix #992 — `new T()` inside a generic body
// without `where T: new()` was a FALSE_ACCEPT (silently zero-initialised
// instead of constructing — the constructor never ran).
//
// Spec authority: §14.1 — "A generic body type-checks under the weakest
// assumption its constraints establish."  `new T()` is permitted ONLY when the
// enclosing generic constrains the parameter `where T: new()`.  The `class`,
// `value`, and type-bound constraints are NOT sufficient.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT: §14.1 / #992 — `new T()` without `where T: new()`
// ---------------------------------------------------------------------------

// §14.1 (#992): `new T()` in a class-level method where the class declares
// only `where T: class` (no `new()`) must be rejected.
BEZEL_TEST(new_type_param_constraint_992, reject_class_level_missing_new_constraint) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public static total_ctors: int := 0; "
        "  public constructor(v: int) Widget.total_ctors := Widget.total_ctors + 1; end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class Box<T> where T: class "
        "  public method MakeOne(): T "
        "    return new T(); "
        "  end MakeOne; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Widget>(); "
        "  var w := b.MakeOne(); "
        "  return Widget.total_ctors; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "14.1") && hasError(r, "992"));
    BEZEL_EXPECT_TRUE(hasError(r, "new()"));
}

// §14.1 (#992): `new T()` in a GENERIC METHOD where the method declares no
// constraints at all (unconstrained T) must be rejected.
BEZEL_TEST(new_type_param_constraint_992, reject_method_level_unconstrained) {
    auto r = parseSemaAll(
        "module M; "
        "static class Factory "
        "  public static method Make<T>(): T "
        "    return new T(); "
        "  end Make; "
        "end Factory; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "14.1") && hasError(r, "992"));
    BEZEL_EXPECT_TRUE(hasError(r, "new()"));
}

// §14.1 (#992): `new T()` where the method declares only `where T: value`
// must be rejected — `value` does not imply constructibility.
BEZEL_TEST(new_type_param_constraint_992, reject_method_level_value_only) {
    auto r = parseSemaAll(
        "module M; "
        "static class Factory "
        "  public static method Make<T>(): T where T: value "
        "    return new T(); "
        "  end Make; "
        "end Factory; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "14.1") && hasError(r, "992"));
    BEZEL_EXPECT_TRUE(hasError(r, "new()"));
}

// ---------------------------------------------------------------------------
// ACCEPT: §14.1 — `new T()` WITH `where T: new()` must remain accepted
// ---------------------------------------------------------------------------

// §14.1 (#992 ACCEPT): class-level `where T: new()` — T has a parameterless
// constructor — `new T()` in the body is valid.
BEZEL_TEST(new_type_param_constraint_992, accept_class_level_with_new_constraint) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class Box<T> where T: new() "
        "  public method MakeOne(): T return new T(); end MakeOne; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Widget>(); "
        "  using var w := b.MakeOne(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "14.1"));
    BEZEL_EXPECT_FALSE(hasError(r, "992"));
}

// §14.1 (#992 ACCEPT): method-level `where T: new()` — `new T()` in the
// method body is valid.
BEZEL_TEST(new_type_param_constraint_992, accept_method_level_with_new_constraint) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "static class Factory "
        "  public static method Make<T>(): int where T: class, new() "
        "    using var t := new T(); return 1; "
        "  end Make; "
        "end Factory; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "14.1"));
    BEZEL_EXPECT_FALSE(hasError(r, "992"));
}

// §14.1 (#992 ACCEPT): record with `where T: new()` — records satisfy new()
// when all fields have defaults; `new T()` in the body is valid.
BEZEL_TEST(new_type_param_constraint_992, accept_record_type_param_with_new_constraint) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Box<T> where T: new() "
        "  public method MakeOne(): T return new T(); end MakeOne; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<Pt>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasError(r, "14.1"));
    BEZEL_EXPECT_FALSE(hasError(r, "992"));
}

// §14.1 (#992 SANITY): ordinary concrete `new Foo()` is completely unaffected
// — the gate only applies when the constructed type is a generic type parameter.
BEZEL_TEST(new_type_param_constraint_992, accept_concrete_new_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
