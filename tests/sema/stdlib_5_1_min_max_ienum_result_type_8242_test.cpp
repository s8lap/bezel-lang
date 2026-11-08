// Regression tests for issue #8242 — the no-selector `Min()` / `Max()` over
// an `IEnumerable<T>` RECEIVER was typed `int` for every element the vtable
// arm did not spell out, so stdlib §5.1's `public method Min(): T` (the
// ELEMENT type) was violated on that one receiver.
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
//     and `TimeOnly`; spec §14.2 *Interface conformance* rule 1 (v0.355)
//     admits a class that lists the interface and rule 2 admits "records and
//     enums implementing it" — so a `string`, a class, a record and a built-in
//     value record all satisfy `where T: IComparable<T>`.
//   * spec §5.1: a return value's type must be IMPLICITLY CONVERTIBLE to the
//     declared return type — the `int` the receiver produced is not
//     convertible to `string`, so the program was rejected for a shape stdlib
//     §5.1 admits.
//
// THE DEFECT (MEASURED on the base, `a939f913`).
//
//   * `F(xs: IEnumerable<string>): string => xs.Min();` was REJECTED —
//     "the returned value has a type that is not implicitly convertible to
//     the target type (§5.1)" — while the CONCRETE (`Array<string>.Min()`)
//     and the LAZY (`Array<string>.Where(…).Min()`) twins typed the element;
//   * the same receiver over a CLASS element and over a built-in value record
//     were rejected the same way.
//
// The receiver's INTERFACE-ness was what broke it: the LAZY arm and the
// CONCRETE arm both keyed the result off the element, and the vtable-backed
// `IEnumerable<T>` arm keyed it off the numeric primitives only.
//
// ORACLE. `runAot` (the AOT path). Every accepting row asserts the EXACT
// STDOUT — a VALUE oracle, never "no diagnostic was produced" — and the
// rejecting rows assert the SPECIFIC diagnostic text, so a program rejected
// for an unrelated reason cannot satisfy a row. The EAGER and the LAZY twin
// of every family are the positive CONTROLS: they show the shape is
// genuinely admitted (and agree with the vtable-backed receiver), rather than
// refused for an unrelated reason.

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

// A program whose `Main` body is `body`.
std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A comparable CLASS element: `class K: IComparable<K>` (spec §14.2 rule 1).
const char* kClassDecl =
    "public class K: IComparable<K>\n"
    "  public X: int;\n"
    "  public constructor(x: int)\n"
    "    X := x;\n"
    "  end;\n"
    "  public method CompareTo(other: K): int => X - other.X;\n"
    "end K;\n";

// The vtable-backed receiver: a static method whose parameter is an
// `IEnumerable<T>`, returning the no-selector `Min()`'s result.
//
// `fBody` is the EXPRESSION `F` returns; it defaults to the bare `xs.Min()`
// (the shape stdlib §5.1 declares). The rows that need a MEMBER of the
// result pass `xs.Min().X` and so also pin that the result really is the
// ELEMENT type and not the `int` the base produced.
std::string ienumMinProgram(const std::string& decls,
                             const std::string& elem,
                             const std::string& retType,
                             const std::string& body,
                             const std::string& fBody = "xs.Min()") {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method F(xs: IEnumerable<" + elem + ">): " +
           retType + " => " + fBody + ";\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The §5.1 conversion diagnostic the `int`-typed result produced (the base's
// rejection of the shape stdlib §5.1 admits).
const char* kConversionNeedle =
    "not implicitly convertible to the target type";

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO, verbatim in shape: the no-selector
// `Min()` over an `IEnumerable<string>` PARAMETER, returned as a `string`.
//
// Pre-fix: the program was REJECTED — "the returned value has a type that is
// not implicitly convertible to the target type (§5.1)" — so the row is RED
// on the base.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           min_over_an_ienumerable_string_parameter_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "import System.Collections.Generic;\n"
            "public class C\n"
            "  public static method F(xs: IEnumerable<string>): string =>"
            " xs.Min();\n"
            "  public static method Main(): int\n"
            "    var a: Array<string> := [\"b\", \"a\"];\n"
            "    Console.WriteLine(C.F(a));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "a\n");
}

// =====================================================================
// ROW 2 — the EAGER twin (the CONCRETE receiver): the same answer, so the
// two receivers AGREE. CONTROL — green on the base too.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           eager_min_over_a_string_element_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<string> := [\"b\", \"a\"];\n"
                 "    Console.WriteLine(a.Min());\n")),
        "a\n");
}

// =====================================================================
// ROW 3 — the LAZY twin: the same answer again. CONTROL — green on the
// base too.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           lazy_min_over_a_string_element_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "    var a: Array<string> := [\"b\", \"a\"];\n"
                 "    Console.WriteLine(a.Where(lambda(x: string) => true)"
                 ".Min());\n")),
        "a\n");
}

// =====================================================================
// ROW 4 — a CLASS element over the `IEnumerable<T>` receiver. The result is
// the CLASS, so the member access `.X` resolves and reads the minimum
// element's field.
//
// The result is READ, not taken: the vtable-backed receiver's element is
// BORROWED from the source array (§28.3 — the array owns its elements), and
// returning it as `owned` would dispose the element a second time when the
// array is disposed. That ownership gap is pre-existing (it is the same for
// `xs.First()` over the same receiver, which this fix does not touch) and is
// filed as a separate follow-up; this row therefore reads the field.
//
// Pre-fix: the program was REJECTED (the `int` result has no member `X`) —
// RED on the base.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           min_over_an_ienumerable_class_parameter_returns_the_class) {
    BEZEL_EXPECT_EQ(
        out(ienumMinProgram(kClassDecl, "K", "int",
                             "    var a: Array<K> := [new K(3), new K(1)];\n"
                             "    Console.WriteLine(C.F(a));\n",
                             "xs.Min().X")),
        "1\n");
}

// =====================================================================
// ROW 5 — a BUILT-IN VALUE RECORD (`DateTime`, stdlib §2.5's
// `IComparable<DateTime>`) over the same receiver. The result is the
// RECORD, so the member access `.Year` resolves.
//
// Pre-fix: REJECTED — RED on the base.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           min_over_an_ienumerable_datetime_parameter_returns_the_record) {
    BEZEL_EXPECT_EQ(
        out(ienumMinProgram("", "DateTime", "DateTime",
                             "    var a: Array<DateTime> := "
                             "[new DateTime(2020, 1, 1), new DateTime(2019, 1, 1)];\n"
                             "    Console.WriteLine(C.F(a).Year);\n")),
        "2019\n");
}

// =====================================================================
// ROW 6 — the SIBLING MEMBER `Max()` over the `IEnumerable<string>`
// receiver: the largest of "b" and "a" is "b". The gate and the typing
// cannot be fixed for `Min()` alone.
//
// Pre-fix: REJECTED — RED on the base.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           max_over_an_ienumerable_string_parameter_returns_the_string) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "import System.Collections.Generic;\n"
            "public class C\n"
            "  public static method F(xs: IEnumerable<string>): string =>"
            " xs.Max();\n"
            "  public static method Main(): int\n"
            "    var a: Array<string> := [\"b\", \"a\"];\n"
            "    Console.WriteLine(C.F(a));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "b\n");
}

// =====================================================================
// ROW 7 — the SHAPE the fix must NOT admit: a class that implements NO
// `IComparable` does not satisfy stdlib §5.1's `where T: IComparable<T>`,
// so the no-selector form is REFUSED with the §5.1 constraint diagnostic
// (the LAZY twin's answer — #8208's gate).
//
// The row is green on the base as well (the base refused it with the bogus
// §5.1 CONVERSION diagnostic); it pins that the fix did not turn the
// vtable-backed receiver into an over-accept.
// =====================================================================
BEZEL_TEST(stdlib_5_1_min_max_ienum_result_type_8242,
           min_over_an_ienumerable_non_comparable_class_is_refused) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(ienumMinProgram("public class P\n"
                                     "  public X: int;\n"
                                     "  public constructor(x: int)\n"
                                     "    X := x;\n"
                                     "  end;\n"
                                     "end P;\n",
                                     "P", "int",
                                     "    var a: Array<P> := [new P(3)];\n"
                                     "    Console.WriteLine(C.F(a));\n"),
                     "does not implement IComparable<T>"));
}
