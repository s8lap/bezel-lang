// Spec-derived tests for §9.6 record inheritance restrictions.
// "A record cannot derive from another record or from a class. A record may
// implement any number of interfaces … Records are implicitly sealed."
// §9.6: a record's base-type list may contain only interfaces; a class or
// another record in the base list is a compile error.

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

// (a) §9.6: record deriving from an abstract class → compile error.
BEZEL_TEST(sema_record_base_restrictions, record_extends_abstract_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "abstract class Base "
        "  public abstract method Compute(): int; "
        "end Base; "
        "record R(Val: int) : Base "
        "  public override method Compute(): int => Val; "
        "end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (b) §9.6: record deriving from a concrete (non-abstract) class → compile error.
BEZEL_TEST(sema_record_base_restrictions, record_extends_concrete_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public method Greet(): int => 42; "
        "end Base; "
        "record R(X: int) : Base "
        "end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (c) §9.6: record deriving from another record → compile error.
BEZEL_TEST(sema_record_base_restrictions, record_extends_record_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record Base(X: int) end Base; "
        "record Derived(X: int, Y: int) : Base "
        "end Derived;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (d) §9.6 CONTROL: record implementing a single interface is valid.
BEZEL_TEST(sema_record_base_restrictions, record_implements_interface_ok) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo "
        "  method GetVal(): int; "
        "end IFoo; "
        "record R(Val: int) : IFoo "
        "  public method GetVal(): int => Val; "
        "end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (e) §9.6 CONTROL: record implementing multiple interfaces is valid.
BEZEL_TEST(sema_record_base_restrictions, record_implements_multiple_interfaces_ok) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo "
        "  method GetA(): int; "
        "end IFoo; "
        "interface IBar "
        "  method GetB(): int; "
        "end IBar; "
        "record R(A: int, B: int) : IFoo, IBar "
        "  public method GetA(): int => A; "
        "  public method GetB(): int => B; "
        "end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (f) §9.6 CONTROL: plain record with no base list compiles cleanly.
BEZEL_TEST(sema_record_base_restrictions, plain_record_no_base_ok) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int) end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (g) §9.6: static class as base of a record → compile error.
BEZEL_TEST(sema_record_base_restrictions, record_extends_static_class_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "static class Util "
        "  public static method Id(x: int): int => x; "
        "end Util; "
        "record R(X: int) : Util "
        "end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
