// Spec-derived tests for Sema slice T4 — named-type assignability
// (§12 subtyping, §5.1, §7). A non-generic named source is assignable
// to a named target when identical or a subclass / implemented
// interface. Generic, null, and Object cases are deferred/allowed.

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

BEZEL_TEST(sema_named, same_type_ok) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C public method M() var x: T := new T(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, derived_to_base_ok) {
    auto r = parseSemaAll(
        "module M; class Base end Base; class Derived : Base end Derived; "
        "class C public method M() var b: Base := new Derived(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, base_to_derived_diagnoses) {
    auto r = parseSemaAll(
        "module M; class Base end Base; class Derived : Base end Derived; "
        "class C public method M() var d: Derived := new Base(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, unrelated_types_diagnose) {
    auto r = parseSemaAll(
        "module M; class A end A; class B end B; "
        "class C public method M() var a: A := new B(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, transitive_subtype_ok) {
    auto r = parseSemaAll(
        "module M; class A end A; class B : A end B; class D : B end D; "
        "class C public method M() var a: A := new D(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, implemented_interface_ok) {
    auto r = parseSemaAll(
        "module M; interface IShape method Area(): int; end IShape; "
        "class Sq : IShape public method Area(): int => 1; end Sq; "
        "class C public method M() var s: IShape := new Sq(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, assign_to_object_ok) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C public method M() var o: Object := new T(); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1178 (§5.1 / §4.6): a mixed pair of a CONCRETE user class / record /
// interface and a CONCRETE primitive (`string` is a primitive in this model)
// has no implicit conversion — it must be rejected, not silently accepted by
// the fall-through. Previously these matched neither the Primitive-Primitive
// nor the Named-Named branch in `checkAssignable` and compiled clean.

BEZEL_TEST(sema_named, class_to_string_assign_rejected_1178) {
    // class -> string (§4.6: no implicit to-string).
    auto r = parseSemaAll(
        "module M; class Dog end Dog; "
        "class C public method M() var s: string := new Dog(); end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, int_to_class_assign_rejected_1178) {
    // int -> class.
    auto r = parseSemaAll(
        "module M; class Dog end Dog; "
        "class C public method M() var d: Dog := 5; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, string_to_class_assign_rejected_1178) {
    // string -> class.
    auto r = parseSemaAll(
        "module M; class Dog end Dog; "
        "class C public method M() var d: Dog := \"hi\"; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, int_to_record_assign_rejected_1178) {
    // int -> record (the reject covers records, not just classes).
    auto r = parseSemaAll(
        "module M; record Point(x: int, y: int); "
        "class C public method M() var p: Point := 5; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Controls that MUST keep compiling (the surgical reject excludes them). ---

BEZEL_TEST(sema_named, null_to_class_still_ok_1178) {
    // The `null` literal (typed `Object?`) into a nullable class reference.
    auto r = parseSemaAll(
        "module M; class Dog end Dog; "
        "class C public method M() var d: Dog? := null; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, null_to_nullable_string_still_ok_1178) {
    // `null` into a nullable string is fine (string is a primitive).
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var s: string? := null; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, int_to_object_box_still_ok_1178) {
    // int -> Object is boxing (an upcast), not a class<->primitive mismatch.
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var o: Object := 5; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_named, generic_param_concrete_assign_still_ok_1178) {
    // A generic type-parameter (`T`, a Named with a null declaration) paired
    // with a concrete type must keep flowing through the fall-through — the
    // monomorphizer resolves it. Assign T<->T inside the body and a concrete
    // result out of a concrete instantiation.
    auto r = parseSemaAll(
        "module M; "
        "class U public static method Id<T>(x: T): T "
        "  var y: T := x; return y; end Id; end U; "
        "class C public method M() var n: int := U.Id<int>(7); end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
