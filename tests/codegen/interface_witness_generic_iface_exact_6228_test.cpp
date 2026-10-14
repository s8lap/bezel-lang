// Regression tests for #6228: the witness search for a GENERIC interface's
// method (`findInterfaceImplAt`, the per-instantiation twin of #6171's
// `findInterfaceImpl`) compared a builtin generic only by head name and
// arity, so at `IL<int>` the slot `F(x: Array<T>)` (= `Array<int>`) could be
// filled by `F(x: Array<string>)`.
//
// Spec §11.2 (v0.338), QUOTED: "parameter types that are exactly equal under
// the interface's substitution" witness the interface member. Each row prints
// the interface-typed calls (and, where useful, the class-typed oracle) and
// asserts the exact stdout.

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

// The interface declares `F(Array<string>)` first; the class declares its
// `Array<string>` overload first. `i.F(Array<int>)` must run the
// `Array<int>` body.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           array_of_t_slot_not_filled_by_array_of_string_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface IL<T> method F(x: Array<string>): int; "
        "method F(x: Array<T>): int; end IL;\n"
        "class C: IL<int> public method F(x: Array<string>): int => 9; "
        "public method F(x: Array<int>): int => 1; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: IL<int> := c; "
        "var a: Array<int> := [1]; Console.WriteLine(i.F(a));\n"
        "  return 0; end Main; end P;\n"),
        "1\n");
}

// The other declaration order: both slots reachable, each runs its own body.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           array_of_t_and_array_of_string_slots_distinct_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface IL<T> method F(x: Array<T>): int; "
        "method F(x: Array<string>): int; end IL;\n"
        "class C: IL<int> public method F(x: Array<int>): int => 1; "
        "public method F(x: Array<string>): int => 9; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: IL<int> := c; "
        "var a: Array<int> := [1]; var s: Array<string> := [\"s\"]; "
        "Console.WriteLine(i.F(a)); Console.WriteLine(i.F(s));\n"
        "  return 0; end Main; end P;\n"),
        "1\n9\n");
}

// Two parameters, the generic one second: interface calls agree with the
// class-typed oracle.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           array_arguments_with_trailing_t_param_match_class_oracle_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I<T> method F(x: Array<string>, y: T): int; "
        "method F(x: Array<int>, y: T): int; end I;\n"
        "class C: I<int> public method F(x: Array<int>, y: int): int => 1; "
        "public method F(x: Array<string>, y: int): int => 9; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: I<int> := c; "
        "var a: Array<int> := [1]; var s: Array<string> := [\"s\"]; "
        "Console.WriteLine(i.F(a, 1)); Console.WriteLine(i.F(s, 1)); "
        "Console.WriteLine(c.F(a, 1)); Console.WriteLine(c.F(s, 1));\n"
        "  return 0; end Main; end P;\n"),
        "1\n9\n1\n9\n");
}
