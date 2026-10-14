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

// Part (2) of #6228: a generic METHOD's own type parameter nested in a builtin
// generic, called through a non-generic interface. The class declares the
// two-parameter overload first; the one-argument call must run the
// one-argument body (the generic-method dispatch matched `Array<V>` against
// `Array<U>` only at a top-level own `<T>`, so the nested pair found no
// candidate and the arity pick took the first declared overload).
// Spec §11.2 (v0.338): the call's arguments select the overload whose
// parameter types match, and §14.2 / §22.6 match a method's own type parameter
// at the same position in the override and the interface member.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           generic_method_own_param_nested_in_builtin_one_arg_call_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I method S<U>(x: Array<U>): int; "
        "method S<U>(x: Array<U>, y: int): int; end I;\n"
        "class C: I public method S<V>(x: Array<V>, y: int): int => 2; "
        "public method S<V>(x: Array<V>): int => 1; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: I := c;\n"
        "  using var a := new Array<int>();\n"
        "  Console.WriteLine(i.S<int>(a).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "1\n");
}

// The same shape with a same-arity sibling `Array<string>` overload declared
// first: the nested `Array<V>` overload is the one `Array<int>` selects.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           generic_method_own_param_nested_same_arity_sibling_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I method S<U>(x: Array<U>): int; end I;\n"
        "class C: I public method S<V>(x: Array<string>): int => 1; "
        "public method S<V>(x: Array<V>): int => 2; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: I := c;\n"
        "  using var a := new Array<int>();\n"
        "  Console.WriteLine(i.S<int>(a).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "2\n");
}

// Both arities and both nested shapes on one interface, against the
// class-typed oracle: 2*10 + 4 - 4 = 20.
BEZEL_TEST(interface_witness_generic_iface_exact_6228,
           generic_method_own_param_nested_both_arities_spec_11_2) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I method S<U>(x: Array<U>): int; "
        "method S<U>(x: Array<U>, y: int): int; end I;\n"
        "class C: I public method S<V>(x: Array<string>, y: int): int => 3; "
        "public method S<V>(x: Array<V>, y: int): int => 4; "
        "public method S<V>(x: Array<V>): int => 2; "
        "public method S<V>(x: Array<string>): int => 1; end C;\n"
        "class P public static method Use(i: I): int\n"
        "  using var a := new Array<int>();\n"
        "  return i.S<int>(a) * 10 + i.S<int>(a, 1) - 4;\n"
        "  end Use;\n"
        "  public static method Main(): int\n"
        "  using var c := new C();\n"
        "  Console.WriteLine(P.Use(c).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "20\n");
}
