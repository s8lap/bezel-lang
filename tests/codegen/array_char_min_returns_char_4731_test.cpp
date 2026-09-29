// Regression tests for #4731 — `Array<char>.Min()` / `.Max()` returned `int`
// rather than `char`, printing 97 (the int value of 'a') where the user
// asked for the smallest character.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5 (v0.286) declares Min/Max under
//     extension IEnumerable<T> where T: IComparable<T>
// and the no-selector forms return `T`. For `Array<char>`, T is `char`,
// and `char` conforms to `IComparable<char>` (the §3.4 keyword-aliased
// primitives carry IComparable conformance as part of their §2.18 contract).
// The result is a `char`, and `Console.WriteLine(char)` renders the single
// code point ("a", not "97").
//
// THE DEFECT WAS A SILENT WIDENING.  Two paths converged on `int`:
//
//   * sema's no-selector Min/Max arm in ExprTypecheck_Calls.cpp fell
//     through to the legacy `int` default for an element type that did
//     not match `ptrComparableElem` (a class/interface/String predicate)
//     or `isNoSelectorAggregationElem` (the eleven INumber conformers).
//     char satisfies neither, so `Array<char>.Min()` was typed `int`.
//
//   * codegen's no-selector numeric Min/Max arm in LlvmBackend_Calls.cpp
//     keyed its accumulator off the element's INumber-membership but did
//     not include Char in the switch — the switch defaulted to Int and
//     the accumulator was built as i32, which the char element then
//     truncated/widened into.
//
// The user-visible symptom: `Array<char>{'c','a','b'}.Min()` printed "97",
// not "a" — `Console.WriteLine(int)` is `%d`, while `Console.WriteLine(char)`
// routes through `bz_char_to_string` (the §9.5 char-rendering path).
//
// SPEC-CITED TEST CASES.  The brief's value-assertion oracle pins the
// post-fix behaviour on stdout: `a\nc\n` (each character rendered as
// itself, NOT its int code point).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + "  return 0; end Main; end C;";
}

}  // namespace

// #4731 / stdlib §5 + spec §29.1 (v0.307): Min() on `Array<char>` must
// return `char` — the post-fix stdout is the bare character "a", NOT the
// int code point "97". The pre-fix stdout was "97\n99\n" (the int values
// of 'a' and 'c'), which is what the brief's value-assertion oracle
// measured.
//
// ⚠ The oracle is on stdoutText byte-for-byte — see CLAUDE.md "feedback-
// codegen-ast-structural-vs-symbol" (cycle-22): accept-fixes must RUN,
// not just compile.
BEZEL_TEST(array_char_min_returns_char_4731, min_returns_char_value) {
    auto r = runAot(main_(
        "  using var arr := new Array<char>(); "
        "  arr.Add('c'); arr.Add('a'); arr.Add('b'); "
        "  Console.WriteLine(arr.Min()); "
        "  Console.WriteLine(arr.Max()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a\nc\n");
}

// #4731 / stdlib §5: the bare `runAotOut` value-channel helper, asserting
// the same post-fix behaviour in the form the harness's single-value
// orchestrator uses. Confirms the fix is independent of how stdout is
// captured (the full stdout path is the load-bearing one).
BEZEL_TEST(array_char_min_returns_char_4731, runAotOut_returns_bare_chars) {
    std::string out = runAotOut(main_(
        "  using var arr := new Array<char>(); "
        "  arr.Add('c'); arr.Add('a'); arr.Add('b'); "
        "  Console.WriteLine(arr.Min()); "
        "  Console.WriteLine(arr.Max()); "));
    BEZEL_EXPECT_EQ(out, "a\nc");
}

// #4731 control: Min/Max on `Array<int>` (a true INumber conformer) must
// still return the element type — the fix must not over-widen a char
// element type into something that breaks the existing int path.
BEZEL_TEST(array_char_min_returns_char_4731, int_path_unchanged) {
    std::string out = runAotOut(main_(
        "  using var arr := new Array<int>(); "
        "  arr.Add(3); arr.Add(1); arr.Add(2); "
        "  Console.WriteLine(arr.Min()); "
        "  Console.WriteLine(arr.Max()); "));
    BEZEL_EXPECT_EQ(out, "1\n3");
}
