// Spec-derived parser tests for null-conditional generic method calls
// (§17: `recv?.Method<TypeArgs>(args)`). #1278: the `<...>(...)` after a
// null-conditional `?.` member was misparsed as a chain of `<`/`>` comparison
// operators, so the legal call form was rejected. The fix applies the same
// generic-call-vs-comparison disambiguation the non-null-conditional
// `recv.Method<T>(args)` path already uses.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

static bool parses(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

// per §17 (#1278): `b?.Get<int>(5)` is a null-conditional generic call, not
// `b ?. Get < int > (5)` comparisons.
BEZEL_TEST(parser_null_cond_generic_1278, null_conditional_generic_call_parses) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Box<T> "
        "  public constructor() end constructor; "
        "  public method Get<U>(u: U): int => 7; "
        "end Box; "
        "public static class P "
        "  public static method Run(b: Box<int>?): int => b?.Get<int>(5) ?? 0; "
        "end P;"));
}

// Multiple type arguments in the null-conditional generic call.
BEZEL_TEST(parser_null_cond_generic_1278, null_conditional_generic_call_two_typeargs_parses) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Pair "
        "  public constructor() end constructor; "
        "  public method Map<A, B>(a: A, b: B): int => 0; "
        "end Pair; "
        "public static class P "
        "  public static method Run(p: Pair?): int => p?.Map<int, int>(1, 2) ?? 0; "
        "end P;"));
}

// per §17.1: a genuine comparison chain still parses as comparisons (the fix
// must not swallow `a < b` when there is no `?.Method` head).
BEZEL_TEST(parser_null_cond_generic_1278, comparison_chain_still_parses) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; public static class P "
        "  public static method Run(a: int, b: int, c: int): bool "
        "    => a < b and b > c; "
        "end P;"));
}

// The non-generic null-conditional call is unaffected.
BEZEL_TEST(parser_null_cond_generic_1278, null_conditional_plain_call_parses) {
    BEZEL_EXPECT_TRUE(parses(
        "module M; "
        "class Box "
        "  public constructor() end constructor; "
        "  public method Get(): int => 7; "
        "end Box; "
        "public static class P "
        "  public static method Run(b: Box?): int => b?.Get() ?? 0; "
        "end P;"));
}
