// Codegen slice CG19 — `is` / `is not` / `as` type tests (§12.7). Each
// object carries a runtime type descriptor (vtable index 0) listing its
// class plus all transitive base types and implemented interfaces. A type
// test walks that descriptor for the target type's identity, so it works
// for exact types, base classes, and interfaces. `as` yields the operand
// when the test holds, else null. Observable via the int exit code (a
// conditional expression turns the bool result into 1/0).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A small hierarchy (Animal ← Dog) plus the IShape interface on Square,
// followed by a Main body. Reused across cases.
int runHier(const std::string& mainBody) {
    return run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 4; end Square; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

} // namespace

BEZEL_TEST(codegen_cg19, is_exact_type) {
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if d is Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_base_class) {
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if d is Animal else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_false_for_subclass_on_base_instance) {
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if a is Dog else 0);"), 0);
}

BEZEL_TEST(codegen_cg19, is_interface) {
    BEZEL_EXPECT_EQ(runHier(
        "var s := new Square(); return (1 if s is IShape else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_not_operator) {
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if a is not Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, is_via_base_typed_parameter) {
    // The static type is Animal, the runtime type is Dog → `is Dog` holds.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C "
        "  public static method Check(a: Animal): int "
        "    => (1 if a is Dog else 0); "
        "  public static method Main(): int "
        "    var d := new Dog(); return Check(d); "
        "  end Main; "
        "end C;"), 1);
}

BEZEL_TEST(codegen_cg19, as_valid_yields_object) {
    // `d as Animal` yields d (a Dog); `(...) is Dog` then holds.
    BEZEL_EXPECT_EQ(runHier(
        "var d := new Dog(); return (1 if (d as Animal) is Dog else 0);"), 1);
}

BEZEL_TEST(codegen_cg19, as_invalid_yields_null) {
    // `a as Dog` yields null (a is only an Animal); `null is Dog` is false.
    BEZEL_EXPECT_EQ(runHier(
        "var a := new Animal(); return (1 if (a as Dog) is Dog else 0);"), 0);
}
