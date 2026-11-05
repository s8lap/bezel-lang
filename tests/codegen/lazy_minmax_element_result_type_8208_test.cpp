// Regression tests for issue #8208 — the LAZY receiver's no-selector
// `Min()` / `Max()` was typed `int` for every element that is not one of the
// eleven §2.19 `INumber<T>` conformers and not a USER RECORD, so
// stdlib §5.1's `public method Min(): T` (the ELEMENT type) was violated.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//     so the no-selector form returns the ELEMENT type `T` — for EVERY `T`
//     that satisfies the constraint, not only for the numeric primitives.
//   * stdlib §2.8 states `string`'s own `IComparable<string>`, and stdlib
//     §2.5 declares `IComparable<Self>` on `DateTime`, `TimeSpan`, `DateOnly`
//     and `TimeOnly`; spec §14.2 *Interface conformance* rule 2 (v0.355)
//     admits "records and enums implementing it" and rule 1 admits a class
//     that lists the interface — so a `string`, a class and a built-in value
//     record all satisfy `where T: IComparable<T>`.
//   * spec §5 / stdlib §9.5: `char` is a §3.4 primitive whose stdlib
//     declaration lists `IComparable<char>`, so the no-selector form's result
//     is the ELEMENT (`char`) — the CONCRETE-collection path has keyed it
//     that way since #4731.
//
// THE DEFECT (MEASURED on r30-c1, the base).
//
//   * `arr.Where(…).Min()` over `Array<string>` PRINTED GARBAGE — 2393792,
//     the string pointer truncated to an int — where the EAGER `arr.Min()`
//     printed "a";
//   * `arr.Where(…).Min().X` over a class element was REFUSED ("no member `X`
//     on primitive type `int`"), and the bare `arr.Where(…).Min()` printed
//     the element's ADDRESS as an int (469591088);
//   * `arr.Where(…).Min().Year` over `Array<DateTime>` was REFUSED ("no member
//     `Year` on primitive type `int`"), where the EAGER `arr.Min().Year`
//     printed 2023;
//   * `arr.Where(…).Min()` over `Array<char>` printed the CODE POINT (97)
//     where the EAGER `arr.Min()` printed 'a'.
//
// The CONCRETE-collection path has always typed the element here (#8092 /
// #8049 / #4731); this is the LAZY receiver's twin. The rows below pin the
// LAZY result types AND their values, with the EAGER twin as the positive
// control in every family — the control shows the shape is genuinely admitted
// rather than refused for an unrelated reason.
//
// ORACLE. `runAot` (the AOT path). Every accepting row asserts the EXACT
// STDOUT — a VALUE oracle, never "no diagnostic was produced" — and the
// rejecting row asserts the SPECIFIC diagnostic text.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// REJECTION with the SPECIFIC diagnostic text: a program that was rejected
// for an unrelated reason (or that compiled) must NOT satisfy the row.
bool rejectsWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The §5.1 constraint diagnostic the no-selector `Min()` / `Max()` gate
// reports (stdlib §5.1 / §14.2 / #1198).
const char* kConstraintNeedle = "does not implement IComparable<T>";

// A comparable CLASS element: `class K: IComparable<K>` (spec §14.2 rule 1).
const char* kClassDecl =
    "public class K: IComparable<K>\n"
    "  public X: int;\n"
    "  public constructor(x: int)\n"
    "    X := x;\n"
    "  end;\n"
    "  public method CompareTo(other: K): int => X - other.X;\n"
    "end K;\n";

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO, `Min()` over a `string` element. The
// result is the STRING ("b" — the smallest of "b" and "c"), not an int.
//
// Pre-fix: the program COMPILED and printed 2393792 — the string pointer
// truncated to an int. The row is RED on the base because the base prints
// that garbage.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_min_over_a_string_element_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<string> := [\"b\", \"a\", \"c\"];\n"
                 "    var m := a.Where(lambda(x: string) => x <> \"a\").Min();\n"
                 "    Console.WriteLine(m);\n")),
        "b\n");
}

// =====================================================================
// ROW 2 — THE SIBLING MEMBER, `Max()` over the same `string` element: the
// largest of "b" and "c" is "c". The gate and the typing cannot be fixed
// for `Min()` alone.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_max_over_a_string_element_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<string> := [\"b\", \"a\", \"c\"];\n"
                 "    var m := a.Where(lambda(x: string) => x <> \"a\").Max();\n"
                 "    Console.WriteLine(m);\n")),
        "c\n");
}

// =====================================================================
// ROW 3 — A CLASS element. The result is the CLASS, so the member access
// `.X` resolves and reads the minimum element's field.
//
// Pre-fix: the program was REFUSED ("no member `X` on primitive type `int`")
// — the row is RED on the base because the base does not compile it.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_min_over_a_class_element_returns_the_class) {
    BEZEL_EXPECT_EQ(
        out(prog(kClassDecl,
                 "    var a: Array<K> := [new K(3), new K(1), new K(2)];\n"
                 "    var m := a.Where(lambda(x: K) => true).Min();\n"
                 "    Console.WriteLine(m.X);\n")),
        "1\n");
}

// =====================================================================
// ROW 4 — THE SIBLING MEMBER over the class element: the maximum's field.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_max_over_a_class_element_returns_the_class) {
    BEZEL_EXPECT_EQ(
        out(prog(kClassDecl,
                 "    var a: Array<K> := [new K(3), new K(1), new K(2)];\n"
                 "    var m := a.Where(lambda(x: K) => true).Max();\n"
                 "    Console.WriteLine(m.X);\n")),
        "3\n");
}

// =====================================================================
// ROW 5 — A `char` element. stdlib §9.5's `Char` record declares
// `IComparable<char>`, so the result is the CHARACTER. The two spellings
// differ visibly: 'a' is 97 as an int.
//
// Pre-fix: the program printed 97 — the code point.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_min_over_a_char_element_returns_the_char) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<char> := ['c', 'a', 'b'];\n"
                 "    var m := a.Where(lambda(x: char) => true).Min();\n"
                 "    Console.WriteLine(m);\n")),
        "a\n");
}

// =====================================================================
// ROW 6 — A BUILT-IN VALUE RECORD (`DateTime`, stdlib §2.5's
// `IComparable<DateTime>`). The result is the RECORD, so the member access
// `.Year` resolves.
//
// Pre-fix: the program was REFUSED ("no member `Year` on primitive type
// `int`").
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_min_over_a_datetime_element_returns_the_record) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<DateTime> := "
                 "[new DateTime(2024, 1, 2), new DateTime(2023, 1, 1)];\n"
                 "    var m := a.Where(lambda(x: DateTime) => true).Min();\n"
                 "    Console.WriteLine(m.Year);\n")),
        "2023\n");
}

// =====================================================================
// CONTROL 1 — THE EAGER TWIN of ROW 1. The concrete-collection path already
// typed the element (#8092), so `arr.Min()` printed "a" (the smallest of the
// three) before this fix and must still print it after. GREEN on the base AND
// on the fix.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           control_eager_min_over_a_string_element_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<string> := [\"b\", \"a\", \"c\"];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m);\n")),
        "a\n");
}

// =====================================================================
// CONTROL 2 — THE EAGER TWIN of ROW 3. GREEN on the base AND on the fix.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           control_eager_min_over_a_class_element_returns_the_class) {
    BEZEL_EXPECT_EQ(
        out(prog(kClassDecl,
                 "    var a: Array<K> := [new K(3), new K(1), new K(2)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.X);\n")),
        "1\n");
}

// =====================================================================
// CONTROL 3 — THE EAGER TWIN of ROW 6. GREEN on the base AND on the fix.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           control_eager_min_over_a_datetime_element_returns_the_record) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<DateTime> := "
                 "[new DateTime(2024, 1, 2), new DateTime(2023, 1, 1)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.Year);\n")),
        "2023\n");
}

// =====================================================================
// CONTROL 4 — AN `int` element is untouched: the §2.19 `INumber<T>` arm
// already keyed the result off the element. GREEN on the base AND on the
// fix.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           control_lazy_min_over_an_int_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<int> := [3, 1, 2];\n"
                 "    var m := a.Where(lambda(x: int) => true).Min();\n"
                 "    Console.WriteLine(m);\n")),
        "1\n");
}

// =====================================================================
// ROW 7 — THE §5.1 CONSTRAINT ON THE LAZY RECEIVER. A CLASS element that
// does NOT satisfy `where T: IComparable<T>` is REFUSED with the §5.1
// diagnostic — the same refusal the CONCRETE-collection path reports, and the
// same wording the LAZY receiver's USER-RECORD arm already reported.
//
// Pre-fix: the LAZY receiver had no class-element gate at all: the call was
// ADMITTED (typed `int`) and codegen refused it with the #2770 wording
// ("its `CompareTo` could not be resolved for dispatch"). The refusal MOVED
// to sema and its wording changed with it — the refusal itself is unchanged.
// =====================================================================
BEZEL_TEST(lazy_minmax_element_result_type_8208,
           lazy_min_over_a_class_with_no_icomparable_is_refused_by_the_gate) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class N\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "end N;\n",
             "    var a: Array<N> := [new N(3), new N(1)];\n"
             "    var m := a.Where(lambda(x: N) => true).Min();\n"
             "    Console.WriteLine(m);\n"),
        kConstraintNeedle));
}
