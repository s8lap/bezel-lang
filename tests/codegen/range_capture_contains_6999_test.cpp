// Spec-derived regression test for issue #6999.
//
// Spec §27.5 (v0.340): a lambda may reference variables from its enclosing
// scope. stdlib §9.8 (v0.340): `Range<T>` declares `pure method
// Contains(value: T): bool`, so a lambda whose expression body is
// `q.Contains(x)` has type `() -> bool`.
//
// THE DEFECT. Sema left the `Range.Contains` call untyped (Range has no
// MethodSymbol), so codegen's lambda return-type derivation fell back to its
// `i32` default while the body emitted `ret i1`: "Function return type does
// not match operand type of return inst" and no executable.
//
// Oracle: `runAotChecked` (ASan armed), exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out6999(const std::string& body) {
    auto r = runAotChecked(
        "module P; public class Program public static method Main(): int " +
        body + " return 0; end Main; end Program;");
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}
}  // namespace

// The issue's repro (§27.5, stdlib §9.8).
BEZEL_TEST(codegen_range_capture_contains_6999, issue_repro) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..3; var f := lambda() => q.Contains(2); "
                "Console.WriteLine(f());"),
        std::string("True\n"));
}

// A parameterised lambda, hit and miss.
BEZEL_TEST(codegen_range_capture_contains_6999, parameter_hit_and_miss) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..3; var f := lambda(x: int) => q.Contains(x); "
                "Console.WriteLine(f(3)); Console.WriteLine(f(4));"),
        std::string("True\nFalse\n"));
}

// Nested lambdas (capture through two closures). The `lambda() => lambda()`
// returning-a-closure shape leaks a 4-byte cell independent of Range (#7031).
BEZEL_TEST(codegen_range_capture_contains_6999, nested_lambda) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..3; var f := lambda() => do "
                "var h := lambda() => q.Contains(3); return h(); end; "
                "Console.WriteLine(f()); "
                "var g := lambda() => f(); Console.WriteLine(g());"),
        std::string("True\nTrue\n"));
}

// `Range<long>`, stepped. Step affects membership only through direction and
// emptiness (stdlib §9.8), so 13 is in 10..14 step 2.
BEZEL_TEST(codegen_range_capture_contains_6999, long_stepped) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 10L..14L step 2L; "
                "var f := lambda(x: long) => q.Contains(x); "
                "Console.WriteLine(f(13L)); Console.WriteLine(f(15L));"),
        std::string("True\nFalse\n"));
}

// `Range<char>`.
BEZEL_TEST(codegen_range_capture_contains_6999, char_range) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 'a'..'e'; var f := lambda(c: char) => q.Contains(c); "
                "Console.WriteLine(f('c')); Console.WriteLine(f('z'));"),
        std::string("True\nFalse\n"));
}

// Descending stepped range.
BEZEL_TEST(codegen_range_capture_contains_6999, descending) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 5..1 step -2; "
                "var f := lambda(x: int) => q.Contains(x); "
                "Console.WriteLine(f(3)); Console.WriteLine(f(6));"),
        std::string("True\nFalse\n"));
}

// Half-open range excludes End (§16.3).
BEZEL_TEST(codegen_range_capture_contains_6999, half_open) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..<3; var f := lambda(x: int) => q.Contains(x); "
                "Console.WriteLine(f(3)); Console.WriteLine(f(2));"),
        std::string("False\nTrue\n"));
}

// Empty range contains nothing (§16.3).
BEZEL_TEST(codegen_range_capture_contains_6999, empty_range) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 3..<3; var f := lambda() => q.Contains(3); "
                "Console.WriteLine(f());"),
        std::string("False\n"));
}

// As a LINQ predicate, and composed with `and` / `not`.
BEZEL_TEST(codegen_range_capture_contains_6999, linq_predicate_and_logic) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..4; "
                "var r := (1..10).Where(lambda(x) => q.Contains(x)).ToArray(); "
                "Console.WriteLine(r.Count); "
                "var g := lambda() => q.Contains(2) and not q.Contains(5); "
                "Console.WriteLine(g());"),
        std::string("4\nTrue\n"));
}

// Controls that already worked: ToString / `in` / for-in over a captured range.
BEZEL_TEST(codegen_range_capture_contains_6999, controls_tostring_in_for) {
    BEZEL_EXPECT_EQ(
        out6999("var q := 1..3; var f := lambda() => 2 in q; "
                "Console.WriteLine(f()); "
                "var h := lambda() => do var s := 0; for var x in q do s += x; "
                "end; return s; end; Console.WriteLine(h()); "
                "var t := lambda() => q.ToString(); Console.WriteLine(t());"),
        std::string("True\n6\nRange(Start: 1, End: 3, Step: 1, "
                    "IsEndInclusive: True, IsOpenEnded: False, "
                    "IsEmpty: False)\n"));
}

// A user record named Range stays non-builtin (§2.8; #6973 control).
BEZEL_TEST(codegen_range_capture_contains_6999, user_range_control) {
    auto r = runAotChecked(
        "module P; record Range(Lo: int, Hi: int); "
        "public class Program public static method Main(): int "
        "var r := new Range(4, 9); var f := lambda() => r.Lo + r.Hi; "
        "Console.WriteLine(f()); return 0; end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("13\n"));
}
