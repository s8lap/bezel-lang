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
