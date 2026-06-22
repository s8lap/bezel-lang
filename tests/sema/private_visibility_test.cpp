// Spec-derived tests for Sema slice S8 — `private` member visibility
// (§13.1). A private member is accessible only from inside its
// declaring type's own bodies.

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

} // namespace

BEZEL_TEST(sema_priv, private_access_from_own_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _x: int := 1; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_access_from_other_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A private _x: int := 1; end A; "
        "class B "
        "  public method M(a: A) _ := a._x; end M; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, public_access_from_other_class_ok) {
    auto r = parseSemaAll(
        "module M; "
        "class A public _x: int := 1; end A; "
        "class B "
        "  public method M(a: A) _ := a._x; end M; "
        "end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_inherited_diagnoses_from_derived) {
    // Per §13.1, `private` is type-local even on inheritance —
    // a derived class cannot reach into the base's privates.
    auto r = parseSemaAll(
        "module M; "
        "class Base private _x: int := 1; end Base; "
        "class Derived : Base "
        "  public method M() _ := this._x; end M; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_priv, private_method_call_from_outside_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class A private method Helper(): int => 1; end A; "
        "class B "
        "  public method M(a: A) _ := a.Helper; end M; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §11.6 (#954): a nested type and its enclosing type have mutual access to each
// other's private members. "A nested class has access to all members
// (including private) of its enclosing class … The enclosing class similarly
// has full access to its nested types' members."

// ACCEPT: a nested class reads the enclosing class's private field.
BEZEL_TEST(sema_priv, nested_class_accesses_enclosing_private_954) {
    auto r = parseSemaAll(
        "module M; "
        "public class Outer "
        "  private _value: int; "
        "  public constructor(v: int) _value := v; end constructor; "
        "  destructor end destructor; "
        "  public class Accessor "
        "    public static method GetValue(o: Outer): int => o._value; "
        "    destructor end destructor; "
        "  end Accessor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: the enclosing class reads a nested type's private field.
BEZEL_TEST(sema_priv, enclosing_class_accesses_nested_private_954) {
    auto r = parseSemaAll(
        "module M; "
        "public class Outer "
        "  public class Inner "
        "    private _secret: int; "
        "    public constructor(v: int) _secret := v; end constructor; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "  public method ExtractSecret(i: Inner): int => i._secret; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: an UNRELATED (non-nested) class still cannot access a private member.
BEZEL_TEST(sema_priv, unrelated_class_private_access_still_diagnoses_954) {
    auto r = parseSemaAll(
        "module M; "
        "public class A private _x: int := 5; destructor end destructor; end A; "
        "public class B public static method Peek(a: A): int => a._x; end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
