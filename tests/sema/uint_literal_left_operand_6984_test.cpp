// fix #6984 (§5.1 "Constant narrowing", §6.2, v0.340).
//
// §5.1 (v0.340): "An expression of integer type that is a compile-time
// constant (§17.14) implicitly converts to any integer type whose range
// contains its value"; §6.2: "the smaller-range operand is implicitly
// promoted to the larger type ... The result has the larger type". An
// integer constant operand adopts the other operand's type, so the result
// type must not depend on which side the constant is written.
//
// Before the fix the result followed the LEFT operand's recorded type, so
// `1 + u` (u: uint) was typed `int` and printed -294967295, and
// `V + U` (V, U integer constants) was rejected as outside `int`'s range.
// The adopting literal was also left typed `int`, so the operation's
// signedness (unsigned only when BOTH operand types are) was signed:
// `u * 3` / `u + 2000000000` missed the uint overflow, `u - 5` wrapped.
//
// Probes: /var/tmp/bezel-scratch/r34-6984/m/ (matrix), p/ (issue repros).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prog(const std::string& members, const std::string& body) {
    return "module M;\nclass P\n" + members +
           "\npublic static method Main(): int\n  " + body +
           "\n  return 0; end Main; end P;\n";
}

std::string prints(const std::string& members, const std::string& body) {
    auto r = runAot(prog(members, body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

bool rejected(const std::string& members, const std::string& body,
              const char* needle) {
    auto r = runAot(prog(members, body));
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, needle);
}

}  // namespace

// The issue's repro: literal left and right agree.
BEZEL_TEST(uint_literal_left_operand_6984,
           literal_left_of_uint_types_result_uint_spec_6_2) {
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "Console.WriteLine(1 + u); "
                               "Console.WriteLine(u + 1);"),
                    std::string("4000000001\n4000000001\n"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "Console.WriteLine(255 | u); "
                               "Console.WriteLine(255 ^ u); "
                               "Console.WriteLine(u | 255);"),
                    std::string("4000000255\n4000000255\n4000000255\n"));
    BEZEL_EXPECT_EQ(prints("", "var u: ulong := 18000000000000000000UL; "
                               "Console.WriteLine(3 + u); "
                               "Console.WriteLine(u + 3);"),
                    std::string("18000000000000000003\n18000000000000000003\n"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "var z: uint := 1 + u; Console.WriteLine(z); "
                               "Console.WriteLine((1 + u) / 2); "
                               "Console.WriteLine(1 + u > 3);"),
                    std::string("4000000001\n2000000000\nTrue\n"));
}

// A literal that adopts uint makes the operation unsigned: the uint overflow
// of §6.1 is raised on either side, and 3 - u / u - 5 no longer wrap.
BEZEL_TEST(uint_literal_left_operand_6984,
           adopting_literal_makes_operation_unsigned_spec_6_1) {
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "Console.WriteLine(2000000000 + u);"),
                    std::string("<exit 70>"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "Console.WriteLine(u + 2000000000);"),
                    std::string("<exit 70>"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 5U; "
                               "Console.WriteLine(3 - u);"),
                    std::string("<exit 70>"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 3U; "
                               "Console.WriteLine(u - 5);"),
                    std::string("<exit 70>"));
    BEZEL_EXPECT_EQ(prints("", "var u: ulong := 18000000000000000000UL; "
                               "Console.WriteLine(u * 3);"),
                    std::string("<exit 70>"));
    BEZEL_EXPECT_EQ(prints("", "var u: uint := 4000000000U; "
                               "Console.WriteLine(u * 1); "
                               "Console.WriteLine(3 - 1 + u);"),
                    std::string("4000000000\n4000000002\n"));
}

// The same constant-narrowing rule for a named constant on the left.
BEZEL_TEST(uint_literal_left_operand_6984,
           named_constant_left_of_uint_types_result_uint_spec_5_1) {
    BEZEL_EXPECT_EQ(prints("", "const V: int := 3; "
                               "const U: uint := 4000000000U; "
                               "var x := V + U; Console.WriteLine(x); "
                               "var y := U + V; Console.WriteLine(y);"),
                    std::string("4000000003\n4000000003\n"));
    BEZEL_EXPECT_EQ(prints("", "const U: uint := 4000000000U; "
                               "var x := 1 + U; Console.WriteLine(x);"),
                    std::string("4000000001\n"));
    BEZEL_EXPECT_EQ(prints("", "const U: uint := 5U; const V: int := -7; "
                               "var x := U + V; Console.WriteLine(x); "
                               "var y := V + U; Console.WriteLine(y);"),
                    std::string("-2\n-2\n"));
    BEZEL_EXPECT_EQ(prints("", "const K: int := 7; "
                               "var u: ulong := 18000000000000000000UL; "
                               "Console.WriteLine(K < u); "
                               "Console.WriteLine(K + u);"),
                    std::string("True\n18000000000000000007\n"));
    // A class-level constant initializer.
    BEZEL_EXPECT_EQ(prints("public const U: uint := 4000000000U;\n"
                           "public const W: uint := 1 + U;\n",
                           "Console.WriteLine(P.W);"),
                    std::string("4000000001\n"));
}

// V - U is a compile error (the result type is uint and -2000000000 is out
// of range), not a run-time OverflowException (spec §6.1, §17.14).
BEZEL_TEST(uint_literal_left_operand_6984,
           constant_overflow_is_compile_error_spec_17_14) {
    BEZEL_EXPECT_TRUE(rejected("", "const V: int := 2000000000; "
                              "const U: uint := 4000000000U; "
                              "var x := V - U; Console.WriteLine(x);",
                          "outside `uint`'s range"));
    BEZEL_EXPECT_TRUE(rejected("", "const V: int := 2000000000; "
                              "const U: uint := 4000000000U; "
                              "var x := V + U; Console.WriteLine(x);",
                          "outside `uint`'s range"));
}

// A negative literal still does not fit uint/ulong (§5.1: "a constant whose
// value does not fit the target's range is a compile error"), either side.
BEZEL_TEST(uint_literal_left_operand_6984,
           negative_literal_still_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejected("", "var u: uint := 3U; Console.WriteLine(-1 + u);",
                          "does not fit `uint`"));
    BEZEL_EXPECT_TRUE(rejected("", "var u: uint := 3U; Console.WriteLine(u + -1);",
                          "does not fit `uint`"));
    BEZEL_EXPECT_TRUE(rejected("", "var u: ulong := 3UL; Console.WriteLine(-1 + u);",
                          "does not fit `ulong`"));
    BEZEL_EXPECT_TRUE(rejected("", "var u: uint := 3U; Console.WriteLine(-1 < u);",
                          "does not fit `uint`"));
}

// Two literals: the unsuffixed one adopts, in either order, so the suffixed
// literal keeps its type (`3UL + 2` stays ulong, not int).
BEZEL_TEST(uint_literal_left_operand_6984,
           two_literals_unsuffixed_adopts_spec_5_1) {
    BEZEL_EXPECT_EQ(prints("", "Console.WriteLine(3UL * 2); "
                               "Console.WriteLine(2 * 3UL); "
                               "Console.WriteLine(18000000000000000000UL / 2); "
                               "Console.WriteLine(1 + 18000000000000000000UL);"),
                    std::string("6\n6\n9000000000000000000\n"
                                "18000000000000000001\n"));
    // `3U + -1`: -1 does not fit uint, 3U fits int, so the one that fits adopts.
    BEZEL_EXPECT_EQ(prints("", "Console.WriteLine(3U + -1);"),
                    std::string("2\n"));
}
