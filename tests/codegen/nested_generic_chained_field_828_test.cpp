// fix #828 / §14.1: chained field access through a NESTED generic instantiation
// (`Box<Box<int>>`, `Box<Box<char>>`, …) miscompiled. fieldAddr's
// "unresolved generic" heuristic flagged any substitution whose lowered LLVM
// type was i32 — but a concrete `int` arg also lowers to i32, so for the inner
// `Box<int>` instance it mistook the legitimate `T = int` for an unresolved
// parameter and overrode it with the OUTER local's substitution (`T → ptr`).
// The inner field then loaded at ptr width → "Function return type does not
// match operand type" IR-verification failure (bug05) or an llvm::cast<Integer>
// assertion abort for `Box<Box<char>>` / `Box<Box<uint>>` (bug06). The fix keys
// "unresolved" off the SEMA arg being a bare type-parameter reference.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// A simple generic Box<T> with one field of type T.
const char* kBox =
    "module M; class Box<T> "
    "  public _value: T; "
    "  public constructor(v: T) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; class C ";
}  // namespace

// Box<Box<int>>: outer._value._value reads the inner i32 field (was ret-ptr IR
// failure).
BEZEL_TEST(nested_generic_chained_field_828, box_box_int) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var outer := new Box<Box<int>>(new Box<int>(42)); "
        "  return outer._value._value; "
        "end Main; end C;"), 42);
}

// Box<Box<char>>: the inner field is a char (was an llvm::cast<IntegerType>
// assertion abort).
BEZEL_TEST(nested_generic_chained_field_828, box_box_char) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var outer := new Box<Box<char>>(new Box<char>('A')); "
        "  return int(outer._value._value); "
        "end Main; end C;"), 65);
}

// Box<Box<uint>>: the inner field is an unsigned int (same abort class as char).
BEZEL_TEST(nested_generic_chained_field_828, box_box_uint) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var outer := new Box<Box<uint>>(new Box<uint>(7u)); "
        "  return int(outer._value._value); "
        "end Main; end C;"), 7);
}

// Triple nesting Box<Box<Box<int>>>.
BEZEL_TEST(nested_generic_chained_field_828, box_box_box_int) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var o := new Box<Box<Box<int>>>("
        "    new Box<Box<int>>(new Box<int>(7))); "
        "  return o._value._value._value; "
        "end Main; end C;"), 7);
}

// Box<Box<string>>: the inner field is a reference (ptr) type — reading
// .Length through the chain still works.
BEZEL_TEST(nested_generic_chained_field_828, box_box_string) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var o := new Box<Box<string>>(new Box<string>(\"hi\")); "
        "  return o._value._value.Length; "
        "end Main; end C;"), 2);
}

// Regression: a single non-nested Box<int> field read still works.
BEZEL_TEST(nested_generic_chained_field_828, single_box_int_regression) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var o := new Box<int>(9); return o._value; "
        "end Main; end C;"), 9);
}

// Splitting the access into two statements (outer then inner) also works.
BEZEL_TEST(nested_generic_chained_field_828, split_outer_then_inner) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  using var o := new Box<Box<int>>(new Box<int>(5)); "
        "  var inner := o._value; return inner._value; "
        "end Main; end C;"), 5);
}
