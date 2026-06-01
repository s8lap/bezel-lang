// Codegen: basic generic class support with field-level substitution.
// For classes with type parameter T, field types are substituted with
// the concrete argument. Methods still use T→i32 fallback (proper
// method monomorphization is a later slice).
//
// Per §14: `class Box<T> { value: T; ... }` is a generic class.
// `new Box<int>(42)` instantiates with T=int; `new Box<double>(3.14)` with T=double.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}

// Generic class with int type param — field read/write.
BEZEL_TEST(codegen_generic, int_field_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(42); "
        "  return b.value; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_generic, int_field_write_and_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(10); "
        "  b.value := 99; "
        "  return b.value; "
        "end Main; end C;"), 99);
}

BEZEL_TEST(codegen_generic, two_type_params) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Pair<A, B> "
        "  public first: A; "
        "  public second: B; "
        "  public constructor(a: A, b: B) first := a; second := b; end constructor; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, int>(3, 7); "
        "  return p.first + p.second; "
        "end Main; end C;"), 10);
}

BEZEL_TEST(codegen_generic, separate_int_instantiations) {
    // Two Box<int> instances are independent.
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var a := new Box<int>(5); "
        "  using var b := new Box<int>(7); "
        "  return a.value + b.value; "
        "end Main; end C;"), 12);
}
