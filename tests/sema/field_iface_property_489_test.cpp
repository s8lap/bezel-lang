// Bug #489 — SIGSEGV when a class satisfies an interface PROPERTY contract with
// a FIELD (or field with initializer). Per §11.2 and §23, a field is NOT a
// property: auto-properties are declared with the `property` keyword (§23.1);
// a plain field declaration (`Name: Type := init`) is a FieldDecl, not a
// PropertyDecl. The §11.2 "matching" rule for interface conformance requires a
// matching *property* member; a field does not satisfy a property contract.
// The compiler must report a clean conformance error — NOT SIGSEGV.
//
// Tests:
//   1. Repro from #489: class with a public field, interface declares property
//      of the same name and type → conformance error (no crash).
//   2. Field with no initializer (same name/type) → conformance error.
//   3. Readonly field → conformance error (readonly fields also do not satisfy).
//   4. CONTROL — class with matching `property` declaration → no error.
//   5. CONTROL — class with matching read-write `property` satisfying a
//      read-write interface property → no error.
//   6. Multiple interface properties; class provides only a field for one →
//      conformance error for that member.
//   7. Inherited field in a derived class does NOT satisfy interface property
//      declared on the interface implemented by the derived class → error.

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

// Returns true if any diagnostic message contains the given substring.
bool anyDiagContains(const bezel::test::SemaResult& r,
                     std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(needle)) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §11.2 / §23 — test 1: exact repro from bug #489.
// `public Val: int := 0` is a field, not a property; it cannot satisfy
// `interface IFoo property Val: int`. A clean conformance error is required.
BEZEL_TEST(sema_field_iface_property_489, field_with_initializer_does_not_satisfy_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo property Val: int; end IFoo; "
        "class Box : IFoo "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    // Must be an error — no crash.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The error must name something useful (class, interface, or member name).
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "Box") ||
                      anyDiagContains(r, "IFoo") ||
                      anyDiagContains(r, "Val"));
}

// §11.2 / §23 — test 2: field with no initializer, same scenario.
BEZEL_TEST(sema_field_iface_property_489, field_no_initializer_does_not_satisfy_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "interface IBar property Count: int; end IBar; "
        "class Counter : IBar "
        "  public Count: int; "
        "  public constructor() Count := 0; end constructor; "
        "  destructor end destructor; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §11.2 / §23 — test 3: readonly field also does NOT satisfy interface property.
// A `readonly` field has no getter function; it is still a FieldDecl.
BEZEL_TEST(sema_field_iface_property_489, readonly_field_does_not_satisfy_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "interface IReadable readonly property Id: int; end IReadable; "
        "class Item : IReadable "
        "  public readonly Id: int; "
        "  public constructor(i: int) Id := i; end constructor; "
        "  destructor end destructor; "
        "end Item;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §11.2 / §23 — test 4 (CONTROL): a class with a matching AUTO-PROPERTY
// `public property Val: int` DOES satisfy the interface contract → no error.
BEZEL_TEST(sema_field_iface_property_489, auto_property_satisfies_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo property Val: int; end IFoo; "
        "class Box : IFoo "
        "  public property Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.2 / §23 — test 5 (CONTROL): block-form property with get+set satisfies
// a read-write interface property → no error.
BEZEL_TEST(sema_field_iface_property_489, block_property_satisfies_rw_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "interface ICounter property Count: int; end ICounter; "
        "class Box : ICounter "
        "  protected _c: int := 0; "
        "  public property Count: int "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end Count; "
        "  destructor end destructor; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.2 / §23 — test 6: class provides a property for one interface member but
// only a field for another → the field-backed member gives an error.
BEZEL_TEST(sema_field_iface_property_489, partial_field_causes_error) {
    auto r = parseSemaAll(
        "module M; "
        "interface ITwo "
        "  property A: int; "
        "  property B: int; "
        "end ITwo; "
        "class Two : ITwo "
        "  public property A: int := 1; "
        "  public B: int := 2; "         // field — must not satisfy ITwo.B
        "  destructor end destructor; "
        "end Two;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §11.2 / §23 — test 7: a field declared in a BASE class that is then used to
// satisfy an interface property in the DERIVED class is also rejected.
// The interface is implemented by the derived class, not the base.
BEZEL_TEST(sema_field_iface_property_489, inherited_field_does_not_satisfy_derived_iface_property) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Base; "
        "interface IFoo property Val: int; end IFoo; "
        "class Derived : Base, IFoo "
        "  destructor end destructor; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
