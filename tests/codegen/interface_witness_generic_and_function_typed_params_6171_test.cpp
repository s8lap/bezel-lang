// Regression tests for #6171 (narrow follow-up to #5999): a call through an
// INTERFACE-typed receiver ran the first-declared same-arity implementing
// overload when the parameter types differ only in a builtin generic's type
// arguments (`Array<int>` vs `Array<string>`) or in a function type's
// parameter / return types (`(int) -> int` vs `(string) -> int`).
//
// Spec §11.2 (v0.338), QUOTED: "parameter types that are exactly equal under
// the interface's substitution" witness the interface member. §22.6 (v0.338)
// stage 4: the call resolves to the most specific applicable overload, so
// `i.F(a)` with `i: I` reaches the class overload with the equal parameter
// types. Codegen's findInterfaceImpl compared a builtin generic only by head
// name and arity and never compared function types. Each row prints the
// interface-typed calls and the direct (class-typed) oracle and asserts the
// exact stdout.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    return r.stdoutText;
}
}  // namespace

// The issue's p1 repro: builtin generic differing in its type argument.
BEZEL_TEST(interface_witness_generic_fn_params_6171, array_type_argument_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I\n"
        " method F(x: Array<int>): int;\n"
        " method F(x: Array<string>): int;\n"
        "end I;\n"
        "class C: I\n"
        " public method F(x: Array<string>): int => 2;\n"
        " public method F(x: Array<int>): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var i: I := c;\n"
        " using var a := new Array<int>(); using var b := new Array<string>();\n"
        " Console.WriteLine(i.F(a).ToString() + \" \" + i.F(b).ToString() + \" | \" + c.F(a).ToString() + \" \" + c.F(b).ToString());\n"
        " return 0; end Main; end P;\n"),
        "1 2 | 1 2\n");
}

// The issue's p8 repro: Dictionary key/value order matters.
BEZEL_TEST(interface_witness_generic_fn_params_6171, dictionary_argument_order_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I\n"
        " method F(x: Array<int>): int;\n"
        " method F(x: Dictionary<string, int>): int;\n"
        " method F(x: Dictionary<int, string>): int;\n"
        "end I;\n"
        "class C: I\n"
        " public method F(x: Dictionary<int, string>): int => 3;\n"
        " public method F(x: Dictionary<string, int>): int => 2;\n"
        " public method F(x: Array<int>): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var i: I := c;\n"
        " using var a := new Array<int>(); using var d1 := new Dictionary<string,int>(); using var d2 := new Dictionary<int,string>();\n"
        " Console.WriteLine(i.F(a).ToString() + i.F(d1).ToString() + i.F(d2).ToString() + \" | \" + c.F(a).ToString() + c.F(d1).ToString() + c.F(d2).ToString());\n"
        " return 0; end Main; end P;\n"),
        "123 | 123\n");
}

// Nested generic arguments.
BEZEL_TEST(interface_witness_generic_fn_params_6171, nested_generic_arguments_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I\n"
        " method F(x: Array<Array<int>>): int;\n"
        " method F(x: Array<Array<string>>): int;\n"
        "end I;\n"
        "class C: I\n"
        " public method F(x: Array<Array<string>>): int => 2;\n"
        " public method F(x: Array<Array<int>>): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var i: I := c;\n"
        " using var a := new Array<Array<int>>(); using var b := new Array<Array<string>>();\n"
        " Console.WriteLine(i.F(a).ToString() + i.F(b).ToString() + \" | \" + c.F(a).ToString() + c.F(b).ToString());\n"
        " return 0; end Main; end P;\n"),
        "12 | 12\n");
}

// The issue's p3 repro (lambda literals as arguments, so no closure local): function-typed parameters differing in a parameter type.
BEZEL_TEST(interface_witness_generic_fn_params_6171, function_parameter_type_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I\n"
        " method F(x: (int) -> int): int;\n"
        " method F(x: (string) -> int): int;\n"
        "end I;\n"
        "class C: I\n"
        " public method F(x: (string) -> int): int => 2;\n"
        " public method F(x: (int) -> int): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var i: I := c;\n"
        " Console.WriteLine(i.F(lambda(n: int) => n).ToString() + i.F(lambda(s: string) => 1).ToString() + \" | \" + c.F(lambda(n: int) => n).ToString() + c.F(lambda(s: string) => 1).ToString());\n"
        " return 0; end Main; end P;\n"),
        "12 | 12\n");
}

// Function types differing only in the return type.
BEZEL_TEST(interface_witness_generic_fn_params_6171, function_return_type_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I\n"
        " method F(x: (int) -> int): int;\n"
        " method F(x: (int) -> string): int;\n"
        "end I;\n"
        "class C: I\n"
        " public method F(x: (int) -> string): int => 2;\n"
        " public method F(x: (int) -> int): int => 1;\n"
        "end C;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var i: I := c;\n"
        " Console.WriteLine(i.F(lambda(n: int) => n).ToString() + i.F(lambda(n: int) => \"s\").ToString() + \" | \" + c.F(lambda(n: int) => n).ToString() + c.F(lambda(n: int) => \"s\").ToString());\n"
        " return 0; end Main; end P;\n"),
        "12 | 12\n");
}
