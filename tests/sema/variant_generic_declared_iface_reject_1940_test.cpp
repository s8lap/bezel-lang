// Spec-derived tests for §11 / §12.9 / §5.1 (#1940, spec v0.196): a type is
// assignable to a generic *declared* (TypeSymbol-bearing) interface ONLY when it
// nominally conforms — i.e. its declaration IS the target's declaration or a
// subtype of it (§11: conformance is by listing the interface among the bases,
// directly or transitively). This is the DECLARED-interface analogue of #1935
// (which pinned the BUILTIN null-declaration residual): stdlib interfaces such as
// IComparable<T> / IEquatable<T> carry a real TypeSymbol, so they reach the
// Named-Named assignability branch, not the builtin `variantGenericAssignable`
// path #1935 fixed.
//
// The unsound ACCEPT this pins: the Named-Named branch of ExprTC::checkAssignable
// splits on generics and, for the SAME-declaration case, enforces §12.9 variance;
// but for a CROSS-declaration (or arity-mismatch) generic pair it took a
// "conservative skip" `return` WITHOUT ever verifying isSubtype(from, to). So a
// user class assigned to an unrelated generic declared interface — e.g. a bare
// `Box` to `IComparable<Box>` — was accepted. A subsequent interface call would
// then dispatch through an itable the object lacks: runtime type confusion, the
// worst class of hole (an ill-typed program admitted). The non-generic Named-
// Named path just below already performed this isSubtype gate; the generic path
// skipped it.
//
// The rejection must fire ONLY where non-conformance is PROVABLE (no nominal path
// at all). A type that genuinely conforms — directly, via a covariant/
// contravariant generic whose declaration relationship holds regardless of the
// type arguments, or through a user-declared intermediate interface — must stay
// ACCEPTED, with the per-argument variance the conservatively-accepted residual
// #1180/#1935 left. No over-rejection of §12.9 conformance.

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

// True iff an ERROR diagnostic is the §5.1 non-conversion — pins THIS defect
// (the assignment was rejected as not implicitly convertible), not merely "some
// error".
bool hasConversionError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- FAIL-without / PASS-with: non-conforming sources are REJECTED ----

// (1) The issue's canonical repro: a bare `Box` (implements NOTHING) assigned to
// the generic declared interface `IComparable<Box>`. FAILS WITHOUT the fix (the
// cross-declaration generic case took the conservative skip, an unsound accept).
// PASSES WITH it (isSubtype(Box, IComparable) is false → §5.1 reject).
BEZEL_TEST(variant_generic_declared_iface_reject_1940, unrelated_class_to_icomparable_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Box(v: int)\n"
        "end Box;\n"
        "class C\n"
        "  public method M(b: Box)\n"
        "    var c: IComparable<Box> := b;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (2) A SIBLING declared-interface mismatch: `Box` implements `IEquatable<Box>`
// but is assigned to `IComparable<Box>` — a DIFFERENT declared interface it does
// not implement. isSubtype(Box, IComparable) is false (IEquatable is unrelated to
// IComparable) → rejected. Guards that implementing SOME declared generic
// interface does not launder assignment to another.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, iequatable_class_to_icomparable_sibling_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Box(v: int): IEquatable<Box>\n"
        "  public method Equals(other: Box): bool\n"
        "    return true;\n"
        "  end Equals;\n"
        "end Box;\n"
        "class C\n"
        "  public method M(b: Box)\n"
        "    var c: IComparable<Box> := b;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (3) A generic user class assigned to an unrelated generic user interface: both
// sides carry a declaration and generic args, but `MyBag<T>` implements nothing,
// so isSubtype(MyBag, IColl) is false → rejected. The generic-`from` form of the
// cross-declaration skip.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, unrelated_generic_class_to_user_iface_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IColl<T> end IColl;\n"
        "class MyBag<T> end MyBag;\n"
        "class C\n"
        "  public method M(a: MyBag<Dog>)\n"
        "    var x: IColl<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// (4) A user interface assigned to an unrelated SIBLING user interface:
// `IFoo<T>` and `IBar<T>` are independent, and `Impl<T>: IFoo<T>` does not extend
// IBar, so `Impl<Dog>` → `IBar<Dog>` is rejected.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, impl_of_one_user_iface_to_sibling_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IFoo<T> end IFoo;\n"
        "interface IBar<T> end IBar;\n"
        "class Impl<T>: IFoo<T> end Impl;\n"
        "class C\n"
        "  public method M(a: Impl<Dog>)\n"
        "    var x: IBar<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConversionError(r));
}

// ---- Regression guards: legitimate conformance stays ACCEPTED (no over-reject) ----

// (5) A class that GENUINELY implements the declared interface `IComparable<Box>`
// stays assignable to it. Exercises the new gate's ACCEPT path directly:
// isSubtype(Box, IComparable) is true (Box lists IComparable<Box> as a base), so
// the assignment is accepted with the arg variance the residual. (Same shape as
// the #1935 suite's Named-Named control — must remain green.)
BEZEL_TEST(variant_generic_declared_iface_reject_1940, class_implements_icomparable_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Box(v: int): IComparable<Box>\n"
        "  public method CompareTo(other: Box): int\n"
        "    return 0;\n"
        "  end CompareTo;\n"
        "end Box;\n"
        "class C\n"
        "  public method M(b: Box)\n"
        "    var c: IComparable<Box> := b;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) COVARIANCE across declarations: `Box<Dog>` implements `IReadOnly<Dog>`,
// which reference-converts to `IReadOnly<Animal>` (Dog <: Animal, covariant
// §12.9). The declaration relationship (Box <: IReadOnly) holds regardless of
// the type argument, so isSubtype is true → accepted. The new gate must NOT
// over-reject this legitimate covariant assignment.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, covariant_class_to_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "interface IReadOnly<out T> end IReadOnly;\n"
        "class Animal end Animal;\n"
        "sealed class Dog: Animal end Dog;\n"
        "sealed class Box<T>: IReadOnly<T> end Box;\n"
        "class C\n"
        "  public method M(b: Box<Dog>)\n"
        "    var x: IReadOnly<Animal> := b;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) Conformance through a user-declared intermediate interface: `IDerived<T>`
// extends `IBase<T>`, so `IDerived<Dog>` → `IBase<Dog>` is a genuine nominal
// subtype (isSubtype(IDerived, IBase) is true) and stays accepted.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, user_iface_extends_user_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Dog end Dog;\n"
        "interface IBase<T> end IBase;\n"
        "interface IDerived<T>: IBase<T> end IDerived;\n"
        "class C\n"
        "  public method M(a: IDerived<Dog>)\n"
        "    var x: IBase<Dog> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) SAME-declaration covariant variance is unaffected by the cross-declaration
// gate: `IReadOnly<Dog>` → `IReadOnly<Animal>` travels the same-declaration
// §12.9 path (not the new gate) and must stay accepted — a guard that the change
// did not disturb the sibling branch.
BEZEL_TEST(variant_generic_declared_iface_reject_1940, same_decl_covariant_iface_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "interface IReadOnly<out T> end IReadOnly;\n"
        "class Animal end Animal;\n"
        "sealed class Dog: Animal end Dog;\n"
        "class C\n"
        "  public method M(a: IReadOnly<Dog>)\n"
        "    var x: IReadOnly<Animal> := a;\n"
        "  end M;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
