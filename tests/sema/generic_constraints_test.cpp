// Spec-derived tests for generic constraint validation at instantiation
// sites (§14.2 / Appendix C.4). Each constraint kind is exercised in both
// the satisfied (no diagnostic) and violated (diagnosed) direction.
//
// Constraints (§14.2 table):
//   where T: notnull   — non-nullable types only.
//   where T: class     — non-nullable reference types only.
//   where T: value     — value types only.
//   where T: SomeClass — non-nullable subtypes of SomeClass (class only).
//   where T: IFoo      — non-nullable types conforming to IFoo.
//   where T: new()     — types with an accessible parameterless constructor,
//                        or records whose default is well-defined.
//
// Instantiation sites: a `Named<...>` type in any type position (field type
// here) and `new C<...>()`. Both funnel through Type Resolution's
// resolveNamed, so a field-type instantiation exercises the same path.

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

// True iff some emitted diagnostic is a §14.2 constraint-violation message.
// Used so the "violated" tests assert that *this* check fired (not some
// unrelated error), and the "satisfied" tests assert it did not.
bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("does not satisfy the constraint") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// where T: class  (§14.2 — non-nullable reference types only)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, class_with_value_arg_diagnoses_14_2) {
    // `where T: class` instantiated as C<int> — int is a value type.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class private _x: T?; end C; "
        "class U private _u: C<int>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, class_with_class_arg_ok_14_2) {
    // C<SomeClass> — SomeClass is a reference type, satisfies `class`.
    auto r = parseSemaAll(
        "module M; "
        "class SomeClass end SomeClass; "
        "class C<T> where T: class private _x: T?; end C; "
        "class U private _u: C<SomeClass>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, class_with_string_arg_ok_14_2) {
    // §14.2 lists `string` among the reference types admitted by `class`.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class private _x: T?; end C; "
        "class U private _u: C<string>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, class_with_nullable_class_arg_diagnoses_14_2) {
    // A type-bound / class constraint admits only non-nullable substitutions
    // (§14.2): C<SomeClass?> violates `where T: class`.
    auto r = parseSemaAll(
        "module M; "
        "class SomeClass end SomeClass; "
        "class C<T> where T: class private _x: T?; end C; "
        "class U private _u: C<SomeClass?>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, class_via_new_with_value_arg_diagnoses_14_2) {
    // The `new C<int>()` instantiation site is checked identically.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class private _x: T?; end C; "
        "class U public method M() var b := new C<int>(); end M; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// where T: value  (§14.2 — value types only)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, value_with_value_arg_ok_14_2) {
    // C<int> satisfies `where T: value` (int is a primitive value type).
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<int>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, value_with_record_arg_ok_14_2) {
    // §14.2: records are value types.
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<Point>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, value_with_class_arg_diagnoses_14_2) {
    // C<SomeClass> violates `where T: value` (a class is a reference type).
    auto r = parseSemaAll(
        "module M; "
        "class SomeClass end SomeClass; "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<SomeClass>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, value_with_string_arg_diagnoses_14_2) {
    // `string` is a reference type (§14.2), so it violates `value`.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<string>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, value_with_nullable_value_arg_diagnoses_14_2) {
    // §14.2: "A nullable value type (e.g. `int?`) does not satisfy `value`."
    // C<int?> must be rejected even though int is a value type — the nullable
    // form is a distinct type (§7.1).
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<int?>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, value_with_nullable_record_arg_diagnoses_14_2) {
    // §14.2: the nullable form of a record (a value type) does not satisfy
    // `value`. C<Point?> is rejected; the non-nullable C<Point> is accepted
    // (see value_with_record_arg_ok_14_2 above as the control).
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C<T> where T: value private _x: T?; end C; "
        "class U private _u: C<Point?>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// where T: notnull  (§14.2 — non-nullable types only)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, notnull_with_value_arg_ok_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: notnull private _x: T?; end C; "
        "class U private _u: C<int>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, notnull_with_class_arg_ok_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class SomeClass end SomeClass; "
        "class C<T> where T: notnull private _x: T?; end C; "
        "class U private _u: C<SomeClass>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, notnull_with_nullable_arg_diagnoses_14_2) {
    // C<int?> violates `where T: notnull` (nullable value type).
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: notnull private _x: T?; end C; "
        "class U private _u: C<int?>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, notnull_with_nullable_ref_arg_diagnoses_14_2) {
    // §14.2: `where T: notnull` excludes EVERY nullable type — value OR
    // reference. C<SomeClass?> (a nullable reference type) is rejected.
    auto r = parseSemaAll(
        "module M; "
        "class SomeClass end SomeClass; "
        "class C<T> where T: notnull private _x: T?; end C; "
        "class U private _u: C<SomeClass?>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// where T: SomeClass  (§14.2 — non-nullable subtypes of a class)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, class_bound_with_subtype_ok_14_2) {
    // Derived : Base, and C<Derived> with `where T: Base` — Derived is a
    // subtype, so it satisfies the bound.
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base end Derived; "
        "class C<T> where T: Base private _x: T?; end C; "
        "class U private _u: C<Derived>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, class_bound_with_self_ok_14_2) {
    // The bound class itself satisfies `where T: Base`.
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class C<T> where T: Base private _x: T?; end C; "
        "class U private _u: C<Base>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, class_bound_with_unrelated_diagnoses_14_2) {
    // An unrelated class does not satisfy `where T: Base`.
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Other end Other; "
        "class C<T> where T: Base private _x: T?; end C; "
        "class U private _u: C<Other>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// where T: IFoo  (§14.2 — non-nullable conforming types)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, iface_bound_conforming_class_ok_14_2) {
    // A class implementing IFoo conforms (rule 1).
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "class Impl : IFoo public method F() end F; end Impl; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class U private _u: C<Impl>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, iface_bound_conforming_record_ok_14_2) {
    // A record listing IFoo conforms directly (rule 2).
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "record R(X: int) : IFoo public method F() end F; end R; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class U private _u: C<R>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, iface_bound_nonconforming_diagnoses_14_2) {
    // A class not implementing IFoo violates `where T: IFoo`.
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "class Plain end Plain; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class U private _u: C<Plain>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, iface_bound_extending_iface_ok_14_2) {
    // An interface that extends IFoo conforms (rule 3).
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "interface IBar : IFoo method G(); end IBar; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class U private _u: C<IBar>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// where T: new()  (§14.2 — accessible parameterless constructor / record)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, new_with_no_ctor_class_ok_14_2) {
    // A class with no declared constructor has the implicit default
    // parameterless constructor.
    auto r = parseSemaAll(
        "module M; "
        "class Plain end Plain; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<Plain>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, new_with_parameterless_ctor_ok_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class HasCtor public constructor() end constructor; end HasCtor; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<HasCtor>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, new_with_paramful_ctor_diagnoses_14_2) {
    // A class whose only constructor takes parameters does not satisfy new().
    auto r = parseSemaAll(
        "module M; "
        "class NeedsArg public constructor(x: int) end constructor; "
        "end NeedsArg; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<NeedsArg>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, new_with_int_arg_ok_14_2) {
    // §14.2 / §17.11: int has a well-defined default → new() satisfied.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<int>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, new_with_default_record_ok_14_2) {
    // §14.2 worked example: record Point(X: int, Y: int) — every field has a
    // default, so default(Point) is well-defined and new() is satisfied.
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<Point>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, new_with_reffield_record_diagnoses_14_2) {
    // §14.2 / §17.11: a record with a non-nullable reference field has no
    // well-defined default → new() is not satisfied.
    auto r = parseSemaAll(
        "module M; "
        "class Ref end Ref; "
        "record Holder(R: Ref); "
        "class C<T> where T: new() private _x: T?; end C; "
        "class U private _u: C<Holder>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// Combined constraints (§14.2 — `where T: class, new()`)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, combined_class_new_satisfied_ok_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class Good end Good; "
        "class C<T> where T: class, new() private _x: T?; end C; "
        "class U private _u: C<Good>?; end U;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, combined_class_new_value_arg_diagnoses_14_2) {
    // C<int> violates the `class` half of `where T: class, new()`.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class, new() private _x: T?; end C; "
        "class U private _u: C<int>?; end U;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// Generic-parameter arguments are deferred (§14.2 — classification only
// known at the outer instantiation).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, generic_param_arg_deferred_14_2) {
    // Inside Wrapper<U>, the instantiation C<U> cannot be classified here;
    // no diagnostic should fire (the obligation is on the outer site). With
    // no concrete instantiation of Wrapper anywhere, the obligation never
    // joins a binding and no diagnostic is produced.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class private _x: T?; end C; "
        "class Wrapper<U> private _c: C<U>?; end Wrapper;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// Nested-generic constraint propagation (§14.2). A constraint on an inner
// generic (`C<T> where T: ...`) must hold for the *concrete* type a nesting
// generic binds its parameter to. `class Box<W> ... C<W> ...` defers W's
// classification to Box's instantiation sites; `Box<IFoo>` binds W=IFoo, so
// `C<W=IFoo>` is checked — and rejected — exactly as the direct form
// `C<IFoo>` would be. Closes the soundness hole where the indirect form was
// silently accepted.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_generic_constraints, nested_new_indirect_violation_diagnoses_14_2) {
    // Box<IFoo> ⇒ C<W=IFoo>; IFoo is an interface and does not satisfy new().
    // The indirect form must be rejected, mirroring the direct C<IFoo>.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "interface IFoo end IFoo; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<IFoo>?; end App;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_new_direct_violation_diagnoses_14_2) {
    // The direct form C<IFoo> (control): already rejected — IFoo (interface)
    // does not satisfy where T: new().
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "interface IFoo end IFoo; "
        "class App private _u: C<IFoo>?; end App;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_new_indirect_satisfied_ok_14_2) {
    // Box<Good> ⇒ C<W=Good>; Good is a class with an implicit default ctor,
    // so it satisfies where T: new(). The whole program compiles clean.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class Good end Good; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<Good>?; end App;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, nested_class_indirect_violation_diagnoses_14_2) {
    // The propagation is constraint-kind agnostic: Box<int> ⇒ C<W=int> with
    // where T: class is rejected (int is a value type).
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: class private _x: T?; end C; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<int>?; end App;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_iface_bound_indirect_violation_diagnoses_14_2) {
    // Box<Plain> ⇒ C<W=Plain> with where T: IFoo; Plain does not conform.
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "class Plain end Plain; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<Plain>?; end App;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_indirect_source_order_independent_14_2) {
    // The outer instantiation (App) precedes both the nesting generic (Box)
    // and the constrained generic (C) in source order. The final join is
    // order-independent, so the violation is still caught.
    auto r = parseSemaAll(
        "module M; "
        "class App private _u: Box<IFoo>?; end App; "
        "class Box<W> private _b: C<W>?; end Box; "
        "interface IFoo end IFoo; "
        "class C<T> where T: new() private _x: T?; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_chained_generic_param_satisfied_ok_14_2) {
    // Outer<X> binds Box<X>, and Box<W> ⇒ C<W>. With Outer<int> ⇒ Box<int>
    // ⇒ C<int> and where T: new(), int satisfies new(): no diagnostic. This
    // exercises the chained-binding path (Box's binding is itself a generic
    // param X, re-keyed to Outer's instantiation).
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class Outer<X> private _o: Box<X>?; end Outer; "
        "class App private _u: Outer<int>?; end App;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_chained_generic_param_violation_diagnoses_14_2) {
    // The chain propagates violations too: Outer<IFoo> ⇒ Box<IFoo> ⇒
    // C<W=IFoo>, and IFoo (interface) does not satisfy where T: new(). The
    // worklist re-keys C's obligation through Box's W to Outer's X and
    // discharges it at Outer<IFoo>.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: new() private _x: T?; end C; "
        "interface IFoo end IFoo; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class Outer<X> private _o: Box<X>?; end Outer; "
        "class App private _u: Outer<IFoo>?; end App;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

BEZEL_TEST(sema_generic_constraints, nested_value_indirect_satisfied_ok_14_2) {
    // Guard against over-rejection: Box<int> ⇒ C<W=int> with where T: value;
    // int is a value type and satisfies it. No diagnostic.
    auto r = parseSemaAll(
        "module M; "
        "class C<T> where T: value private _x: T?; end C; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<int>?; end App;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_generic_constraints, nested_iface_bound_indirect_satisfied_ok_14_2) {
    // Guard against over-rejection: Box<Impl> ⇒ C<W=Impl> with where T: IFoo;
    // Impl implements IFoo (rule 1), so it conforms. No diagnostic.
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(); end IFoo; "
        "class Impl : IFoo public method F() end F; end Impl; "
        "class C<T> where T: IFoo private _x: T?; end C; "
        "class Box<W> private _b: C<W>?; end Box; "
        "class App private _u: Box<Impl>?; end App;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
