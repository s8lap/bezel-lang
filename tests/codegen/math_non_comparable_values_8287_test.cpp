// Regression tests for issue #8287 -- `Math.Min` / `Max` / `Clamp` (stdlib
// §2.2, v0.355: `where T: IComparable<T>`) admitted a `bool` and an `IntPtr`
// argument, although neither type declares `IComparable<T>`:
//   * stdlib §9.4: `public record Boolean: IEquatable<bool>` -- no IComparable;
//   * stdlib §10.0: `public record IntPtr` -- no base list at all.
// Spec §14.2: a type argument must satisfy its constraint, so each call is a
// compile error naming the unsatisfied `IComparable<T>` constraint -- the same
// diagnostic a user non-comparable record gets (#7935's control row).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// True when the program was REJECTED with a diagnostic carrying `needle`.
bool rejectedWith(const std::string& program, const std::string& needle) {
    auto r = runAotChecked(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// The issue's r06b repro: inferred T = bool.
BEZEL_TEST(math_non_comparable_values_8287, min_over_bools_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var m := Math.Min(true, false); Console.WriteLine(m);"),
        "type argument `bool` does not satisfy the constraint `where T: "
        "IComparable<T>`"));
}

// r06: explicit T = Boolean.
BEZEL_TEST(math_non_comparable_values_8287, max_explicit_boolean_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var m := Math.Max<Boolean>(true, false);"),
        "type argument `bool` does not satisfy the constraint `where T: "
        "IComparable<T>`"));
}

BEZEL_TEST(math_non_comparable_values_8287, clamp_over_bools_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var m := Math.Clamp(true, false, true);"),
        "type argument `bool` does not satisfy the constraint `where T: "
        "IComparable<T>`"));
}

// r09: IntPtr.
BEZEL_TEST(math_non_comparable_values_8287, max_over_intptr_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var a := IntPtr.Zero; var b := IntPtr.Zero; "
             "var m := Math.Max(a, b);"),
        "type argument `IntPtr` does not satisfy the constraint `where T: "
        "IComparable<T>`"));
}

BEZEL_TEST(math_non_comparable_values_8287, min_and_clamp_over_intptr_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var a := IntPtr.Zero; var m := Math.Min(a, a);"),
        "type argument `IntPtr` does not satisfy the constraint"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("var a := IntPtr.Zero; var m := Math.Clamp(a, a, a);"),
        "type argument `IntPtr` does not satisfy the constraint"));
}

// CONTROLS -- every comparable type is unchanged.
BEZEL_TEST(math_non_comparable_values_8287, control_primitives_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "Console.WriteLine(Math.Max(3, 9)); "
            "Console.WriteLine(Math.Min(3L, 9L)); "
            "Console.WriteLine(Math.Max(1.5, 2.5)); "
            "Console.WriteLine(Math.Min(\"b\", \"a\")); "
            "Console.WriteLine(Math.Clamp(15, 0, 10));")),
        std::string("9\n3\n2.5\na\n10\n"));
}

BEZEL_TEST(math_non_comparable_values_8287, control_user_icomparable_record) {
    BEZEL_EXPECT_EQ(
        outChecked(
            "module M; public record P(X: int): IComparable<P> "
            "  public method CompareTo(other: P): int "
            "    if X < other.X then return -1; end if; "
            "    if X > other.X then return 1; end if; "
            "    return 0; "
            "  end CompareTo; "
            "end P; "
            "class C public static method Main(): int "
            "var a := new P(1); var b := new P(3); "
            "Console.WriteLine(Math.Max(a, b).X); "
            "return 0; end Main; end C;"),
        std::string("3\n"));
}

BEZEL_TEST(math_non_comparable_values_8287, control_guid_in_math_still_works) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var g1 := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2 := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Max(g1, g2).ToString());")),
        std::string("22222222-2222-2222-2222-222222222222\n"));
}
