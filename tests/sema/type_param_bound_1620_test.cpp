// Spec-derived sema tests for fix #1620 — `where T: U` type-parameter bounds.
//
// Authority: §14.2 "Type-parameter bounds (`where T: U`)" (v0.182): "a
// substitution S for T satisfies `where T: U` exactly when S is non-nullable
// and is assignable to the substitution for U ... The check runs at
// instantiation, after both parameters are substituted. Inside the body, a
// T-typed value is usable in any U-typed position, and T conforms to every
// interface that U's own constraints guarantee."
//
// Two gaps were closed:
//   Gap 1 — inside the body, `T` did NOT pick up `U`'s member surface (a
//           `T`-typed value calling a `U`-constraint member was rejected as an
//           "unconstrained type parameter").
//   Gap 2 — at an instantiation site the `where T: U` assignability was NOT
//           checked, so an unrelated substitution (T=Cat, U=Animal) was
//           silently accepted (a soundness hole).

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

bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("14.2") != std::string::npos)
            return true;
    return false;
}

bool hasUnconstrainedParamError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("unconstrained type parameter") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gap 1 — member-surface propagation: inside the body, a `T`-typed value sees
// the members guaranteed by `U`'s own constraint (`where T: U where U: Animal`).
// ---------------------------------------------------------------------------

// §14.2: `x.Speak()` on a `T`-typed value resolves through `where T: U` to
// `U`'s `Animal` constraint surface — accepted, no unconstrained-param error.
BEZEL_TEST(type_param_bound_1620, member_surface_through_u_chain_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public method Speak(): string return \"...\"; end Speak; "
        "end Animal; "
        "class Dog: Animal end Dog; "
        "static class Utils "
        "  public static method Describe<T, U>(x: T): string "
        "      where T: U where U: Animal "
        "    return x.Speak(); "
        "  end Describe; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasUnconstrainedParamError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 ANTI-OVER-REJECT: with `U` unconstrained, `T` gets only Object's
// surface — a non-Object member on `T` stays rejected (the chain adds nothing).
BEZEL_TEST(type_param_bound_1620, member_surface_unconstrained_u_still_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public method Speak(): string return \"...\"; end Speak; "
        "end Animal; "
        "static class Utils "
        "  public static method Describe<T, U>(x: T): string "
        "      where T: U "
        "    return x.Speak(); "
        "  end Describe; "
        "end Utils; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasUnconstrainedParamError(r));
}

// ---------------------------------------------------------------------------
// Gap 2 — instantiation assignability at a generic-METHOD call site.
// ---------------------------------------------------------------------------

// §14.2: `AsAnimal<Cat, Animal>` — `Cat` is not assignable to `Animal`, so the
// `where T: U` (U := Animal) obligation is UNSATISFIED — rejected.
BEZEL_TEST(type_param_bound_1620, method_unsound_instantiation_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Cat end Cat; "
        "static class Utils "
        "  public static method Ident<T, U>(x: T): int "
        "      where T: U where U: Animal "
        "    return 0; "
        "  end Ident; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var c := new Cat(); return Utils.Ident<Cat, Animal>(c); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 ANTI-OVER-REJECT: `AsAnimal<Dog, Animal>` — `Dog` IS a subclass of
// `Animal`, so `where T: U` (U := Animal) is satisfied — accepted.
BEZEL_TEST(type_param_bound_1620, method_valid_instantiation_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "static class Utils "
        "  public static method Ident<T, U>(x: T): int "
        "      where T: U where U: Animal "
        "    return 0; "
        "  end Ident; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); return Utils.Ident<Dog, Animal>(d); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// Gap 2 — instantiation assignability at a generic-TYPE instantiation site.
// ---------------------------------------------------------------------------

// §14.2: `Box<Cat, Animal>` — `Cat` is not assignable to `Animal`, so the
// `where T: U` (U := Animal) obligation is UNSATISFIED — rejected.
BEZEL_TEST(type_param_bound_1620, type_unsound_instantiation_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Cat end Cat; "
        "class Box<T, U> where T: U where U: Animal "
        "  public item: T; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b: Box<Cat, Animal>? := null; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 ANTI-OVER-REJECT: `Box<Dog, Animal>` — `Dog` IS a subclass of
// `Animal`, so `where T: U` (U := Animal) is satisfied — accepted.
BEZEL_TEST(type_param_bound_1620, type_valid_instantiation_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Box<T, U> where T: U where U: Animal "
        "  public item: T; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b: Box<Dog, Animal>? := null; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}
