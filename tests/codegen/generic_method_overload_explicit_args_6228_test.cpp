// Regression tests for #6228 residuals (after f39e9276): overload selection
// for a GENERIC method's own type parameters.
//
// (1) A direct call that writes its method type arguments (`c.S<int>(b)`)
//     ranked each candidate with its UNSUBSTITUTED signature: the bare `V` of
//     `S<V>(x: Array<V>)` and `S<V>(x: Array<Array<V>>)` both failed against
//     `Array<Array<int>>`, and the arity pick took the first declaration.
// (2) Through a non-generic interface, a generic method whose function-typed
//     formal is spelled with its own type parameter (`T<V>(x: (V) -> int)`)
//     never matched its interface slot `T<U>(x: (U) -> int)`: the
//     declaration walk recursed into Named types only, so the arity pick ran
//     the first same-name overload.
//
// Spec §22.6 (v0.338), "Generic candidates": "Surviving candidates enter the
// ranking below with their substituted signatures"; the explicit type
// arguments are the substitution (§14.5: no inference runs when they are
// written). Spec §14.2 / §22.6: a method's own type parameter is matched at
// the same position in an override and in the interface member.
//
// Expected values were derived by hand from those clauses before the fix.

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

// Explicit type argument, nested own parameter: `S<int>` substitutes into
// `Array<V>` (= Array<int>, does not accept Array<Array<int>>) and
// `Array<Array<V>>` (accepts it), so only the second is applicable: 20.
// The interface-typed call and the class-typed call agree.
BEZEL_TEST(generic_method_overload_explicit_args_6228,
           explicit_type_arg_nested_own_param_picks_applicable_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I method S<U>(x: Array<Array<U>>): int; "
        "method S<U>(x: Array<U>): int; end I;\n"
        "class C: I public method S<V>(x: Array<V>): int => 10; "
        "public method S<V>(x: Array<Array<V>>): int => 20; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: I := c;\n"
        "  using var a := new Array<int>(); "
        "using var b := new Array<Array<int>>();\n"
        "  Console.WriteLine(i.S<int>(b).ToString() + \" \" + "
        "i.S<int>(a).ToString() + \" \" + c.S<int>(b).ToString() + \" \" + "
        "c.S<int>(a).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "20 10 20 10\n");
}

// Explicit type argument on a bare own parameter beside an `Array<V>` one:
// `S<int>(Array<int>)` is only the `Array<V>` overload (the bare `V` is int),
// and `S<int>(3)` only the bare one. Expected "20 10".
BEZEL_TEST(generic_method_overload_explicit_args_6228,
           explicit_type_arg_bare_and_array_overloads_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "class C public method S<V>(x: V): int => 10; "
        "public method S<V>(x: Array<V>): int => 20; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); using var b := new Array<int>();\n"
        "  Console.WriteLine(c.S<int>(b).ToString() + \" \" + "
        "c.S<int>(3).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "20 10\n");
}

// Explicit `<Array<int>>` substitutes the bare `V` to Array<int>, which the
// argument matches, and `Array<V>` to Array<Array<int>>, which it does not:
// the bare overload, 10.
BEZEL_TEST(generic_method_overload_explicit_args_6228,
           explicit_type_arg_builtin_substitutes_bare_param_spec_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "class C public method S<V>(x: V): int => 10; "
        "public method S<V>(x: Array<V>): int => 20; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); using var b := new Array<int>();\n"
        "  Console.WriteLine(c.S<Array<int>>(b).ToString());\n"
        "  return 0; end Main; end P;\n"),
        "10\n");
}

// Part (2): a function-typed formal spelled with the method's own type
// parameter, through a non-generic interface. The interface's `(U) -> int`
// slot is witnessed by the `(V) -> int` overload (20), not by the `(V) ->
// string` overload declared first. The string-returning lambda reaches the
// string overload (10). Class-typed calls agree.
BEZEL_TEST(generic_method_overload_explicit_args_6228,
           function_typed_own_param_interface_witness_spec_11_2_and_22_6) {
    BEZEL_EXPECT_EQ(out(
        "module M;\n"
        "interface I method T<U>(x: (U) -> int): int; "
        "method T<U>(x: (U) -> string): int; end I;\n"
        "class C: I public method T<V>(x: (V) -> string): int => 10; "
        "public method T<V>(x: (V) -> int): int => 20; end C;\n"
        "class P public static method Main(): int\n"
        "  using var c := new C(); var i: I := c;\n"
        "  Console.WriteLine(i.T<int>(lambda(n: int): int => n).ToString() + "
        "\" \" + i.T<int>(lambda(n: int): string => \"s\").ToString() + \" \" + "
        "c.T<int>(lambda(n: int): int => n).ToString() + \" \" + "
        "c.T<int>(lambda(n: int): string => \"s\").ToString());\n"
        "  return 0; end Main; end P;\n"),
        "20 10 20 10\n");
}
