// fix #8168 admitting twins (§27.3 *Admission at a target-typed non-call
// position*, §5.1, v0.355): the fix rejects a lambda whose body type does not
// implicitly convert to the target's return slot (the reject rows live in
// tests/sema/lambda_body_result_conversion_8168_test.cpp). Every pair below
// IS a §5.1 table row, so the lambda stays admitted and the call prints the
// converted value. These rows are green before and after the fix: they guard
// the new rejection against over-reach (a silent-acceptance mirror).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints8168(const std::string& body, const std::string& expected) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n" +
                    body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// §27.3's own example: "`var f: (int) -> long := lambda(x) => x * 2;` is
// admitted, the `int` body widening to `long`".
BEZEL_TEST(lambda_body_result_widening_8168,
           spec_example_int_body_at_long_slot_admitted_spec_27_3) {
    expectPrints8168(
        "    var f: (int) -> long := lambda(x) => x * 2;\n"
        "    Console.WriteLine(f(21));\n",
        "42\n");
}

// The same widening through a block body's `return`.
BEZEL_TEST(lambda_body_result_widening_8168,
           block_body_int_return_at_long_slot_admitted_spec_27_3) {
    expectPrints8168(
        "    var f: (int) -> long := lambda(x: int) => do return x + 1; end;\n"
        "    Console.WriteLine(f(41));\n",
        "42\n");
}

// §5.1 row `uint` -> `ulong`: a value above `int`'s range survives.
BEZEL_TEST(lambda_body_result_widening_8168,
           uint_body_at_ulong_slot_admitted_spec_5_1) {
    expectPrints8168(
        "    var f: (uint) -> ulong := lambda(x: uint) => x;\n"
        "    Console.WriteLine(f(4000000000U));\n",
        "4000000000\n");
}

// §5.1 row `byte` -> `int`: unsigned to strictly larger signed is permitted.
BEZEL_TEST(lambda_body_result_widening_8168,
           byte_body_at_int_slot_admitted_spec_5_1) {
    expectPrints8168(
        "    var f: (byte) -> int := lambda(x: byte) => x;\n"
        "    Console.WriteLine(f(200));\n",
        "200\n");
}

// §5.1 row `int` -> `decimal`.
BEZEL_TEST(lambda_body_result_widening_8168,
           int_body_at_decimal_slot_admitted_spec_5_1) {
    expectPrints8168(
        "    var f: (int) -> decimal := lambda(x: int) => x;\n"
        "    Console.WriteLine(f(7));\n",
        "7\n");
}

// §5.1 row `float` -> `double` (the #8159 widening) stays admitted.
BEZEL_TEST(lambda_body_result_widening_8168,
           float_body_at_double_slot_admitted_spec_5_1) {
    expectPrints8168(
        "    var f: (float) -> double := lambda(x: float) => x;\n"
        "    Console.WriteLine(f(1.5f));\n",
        "1.5\n");
}

// §5.1 *Nullable conversions*: `T` converts to `T?`.
BEZEL_TEST(lambda_body_result_widening_8168,
           int_body_at_nullable_int_slot_admitted_spec_5_1) {
    expectPrints8168(
        "    var f: (int) -> int? := lambda(x: int) => x;\n"
        "    Console.WriteLine(f(9) ?? 0);\n",
        "9\n");
}

// Identity at a non-numeric primitive slot: a `string` body at `string`.
BEZEL_TEST(lambda_body_result_widening_8168,
           string_body_at_string_slot_admitted_spec_27_3) {
    expectPrints8168(
        "    var f: (int) -> string := lambda(x: int) => $\"n={x}\";\n"
        "    Console.WriteLine(f(4));\n",
        "n=4\n");
}
