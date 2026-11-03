// Spec-derived regression test for issue #7167 — `string.Join`'s **items**
// argument was not type-gated, so a program the compiler ACCEPTED read past
// the end of a heap allocation (an out-of-bounds READ of size 8).
//
// THE SPEC.
//
//   stdlib §9.6 (v0.350) declares, verbatim:
//
//     public static pure method Join(separator: string,
//                                     items: IEnumerable<string>): string;
//
//   So `items` is an `IEnumerable<string>`: every element is a `string`, one
//   pointer, eight bytes.  spec §5.1 (v0.350) is the implicit-conversion
//   table, and it has NO row admitting an `Array<int>` or a bare `string` to
//   an `IEnumerable<string>`.  `IEnumerable<T>`'s covariance does not help:
//   stdlib §9.6 declares `public interface IEnumerable<out T>`, so an
//   `IEnumerable<int>` is an `IEnumerable<string>` only if `int` were a
//   supertype of `string` - it is not (spec §12.9).  A `string` is an
//   `IEnumerable<char>`, never an `IEnumerable<string>`.
//
//   spec §9.6 also declares `Trim(chars: IEnumerable<char>): string` and
//   `IsNullOrWhiteSpace(value: string?): bool`, so the same rule reaches
//   their parameters.
//
// THE DEFECT.
//
// The compiler gated `Join`'s SEPARATOR and left `items` ungated, so the
// call was half-gated and an ill-typed items argument reached the lowering.
// The lazy materialiser was drained with a hard-coded 8-byte element width,
// so it copied 8 bytes out of a 4-byte element slot - the AddressSanitizer
// heap-buffer-overflow READ the issue measured:
//
//   string.Join(",", Array<int>)  -> compiled cleanly; READ of size 8
//   string.Join(",", "ab")        -> compiled cleanly; the SAME read
//
// A program that reads out of bounds is the top severity, and the compiler
// ACCEPTED both.
//
// THE ORACLE IS POSITIVE.  Each refusal row asserts the refusal itself, a
// REAL source location, and the POSITIVE diagnostic text - the argument's
// name and the declared type it must have, plus the §5.1 citation.  The
// control row asserts the JOINED VALUE of a well-typed call.
//
// With the compiler refusing the ill-typed calls there is no reachable
// spelling of the out-of-bounds read, and the runtime half is bounded too:
// the lazy drain's copy width is bounded by the element width the pipeline
// itself holds, so a caller that asks for a wider element gets no snapshot
// at all.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §5.1 refusal at a declared built-in parameter: the program did NOT
// run, the rejection is a located diagnostic at a REAL source location, and
// the diagnostic names the argument, the type it must have, and §5.1.
void expectRefused(const JitResult& r, const char* needle,
                   const char* expectedType) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, expectedType));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

// ===========================================================================
// THE ISSUE'S FIRST SHAPE — an `Array<int>` as the items argument.
//
// stdlib §9.6 declares `items: IEnumerable<string>`; an `Array<int>` is an
// `IEnumerable<int>` (stdlib §4.4), and §5.1 admits no conversion from it
// to an `IEnumerable<string>`.
// ===========================================================================
BEZEL_TEST(string_join_items_gate_7167,
           string_join_refuses_an_int_array_items_argument) {
    auto r = runAot(prog(
        "    var nums: Array<int> := [1, 2, 3]; "
        "    Console.WriteLine(string.Join(\",\", nums)); "));
    expectRefused(r, "the items argument to `string.Join`",
                  "expected `IEnumerable<string>`");
}

// ===========================================================================
// THE ISSUE'S SECOND SHAPE — a bare `string` as the items argument.
//
// A `string` is an `IEnumerable<char>` (stdlib §9.6), never an
// `IEnumerable<string>`, so the same overflow was reachable through a
// WELL-TYPED-looking argument.
// ===========================================================================
BEZEL_TEST(string_join_items_gate_7167,
           string_join_refuses_a_string_items_argument) {
    auto r = runAot(prog("    Console.WriteLine(string.Join(\",\", \"ab\")); "));
    expectRefused(r, "the items argument to `string.Join`",
                  "expected `IEnumerable<string>`");
}

// ===========================================================================
// THE SIBLING THE SAME DECLARED-TYPE RULE REACHES — stdlib §9.6 declares
// `Trim(chars: IEnumerable<char>): string`, so an `int` chars argument is
// refused rather than compiled into a malformed-IR crash.
// ===========================================================================
BEZEL_TEST(string_join_items_gate_7167,
           string_trim_refuses_an_int_chars_argument) {
    auto r = runAot(prog("    Console.WriteLine(\"  a  \".Trim(5)); "));
    expectRefused(r, "no matching overload of `string.Trim`",
                  "the `chars` argument must be an `IEnumerable<char>`");
}

// ===========================================================================
// AND THE OTHER SIBLING — stdlib §9.6 declares
// `IsNullOrWhiteSpace(value: string?): bool`.
// ===========================================================================
BEZEL_TEST(string_join_items_gate_7167,
           string_isnullorwhitespace_refuses_an_int_value_argument) {
    auto r = runAot(prog(
        "    Console.WriteLine(string.IsNullOrWhiteSpace(5)); "));
    expectRefused(r, "no matching overload of `string.IsNullOrWhiteSpace`",
                  "the `value` argument must be a `string`");
}

// ===========================================================================
// THE CONTROL — the well-typed call still JOINS, and the result is the
// joined value.
// ===========================================================================
BEZEL_TEST(string_join_items_gate_7167,
           string_join_of_a_string_array_still_joins) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var ss: Array<string> := [\"a\", \"b\", \"c\"]; "
        "    Console.WriteLine(string.Join(\",\", ss)); ")),
        std::string("a,b,c"));
}
