// Codegen — `Array<T>.IndexOf(item: T, startIndex: int := 0): int` (#4744).
//
// stdlib §4.4 (v0.307) declares:
//
//   public method IndexOf(item: T, startIndex: int := 0): int;
//                                     // first occurrence at or after startIndex, or -1
//                                     //   0 <= startIndex <= Count required,
//                                     //   IndexOutOfRangeException otherwise;
//                                     //   startIndex = Count is legal and returns -1
//
// Pre-fix, sema typed the two-argument spelling as `int` (the `m == "IndexOf"`
// arm in ExprTypecheck_Calls types `Array`/`OwningArray` receivers regardless
// of arity) but NO codegen arm matched it — the array handler in
// LlvmBackend_Calls.cpp was gated on `call.args.size() == 1`. The call
// therefore lowered to nothing: the #3079 "compiles, emits nothing" class.
// `Console.WriteLine(arr.IndexOf(7, 2))` printed NOTHING AT ALL (not even the
// line terminator) and `_ := arr.IndexOf(1, 4)` silently did not raise, so the
// oracles below observe stdout — the pre-fix observation is the empty string,
// which no expectation here matches — rather than merely checking that the
// program compiled or that it exited 0 (it did both, before the fix).
//
// The rows derive from rule-corpus cases l4_4_0003 / l4_4_0004 / l4_4_0005
// (tests/conformance/rules/cases/), which transcribe the stdlib §4.4 sentences
// above one sentence per case.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// stdlib §4.4: "first occurrence at or after startIndex". [7,7,7,9] searched
// for 7 from index 2 is index 2 — the 1-arg form would answer 0, and the
// pre-fix lowering answered nothing at all.
BEZEL_TEST(array_indexof_startindex_4744, finds_first_occurrence_at_or_after_start) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [7, 7, 7, 9]; "
        "  Console.WriteLine(arr.IndexOf(7, 2)); "
        "end Main; end C;"), "2");
}

// stdlib §4.4: a start past every occurrence yields -1 — the element IS in the
// array, so a lowering that ignored startIndex would answer 0 here.
BEZEL_TEST(array_indexof_startindex_4744, start_past_the_only_match_is_minus_one) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [5, 1, 2, 3]; "
        "  Console.WriteLine(arr.IndexOf(5, 1)); "
        "end Main; end C;"), "-1");
}

// stdlib §4.4: "startIndex = Count is legal and returns -1" — the boundary
// separating the legal closed interval [0, Count] from the raise.
BEZEL_TEST(array_indexof_startindex_4744, start_equal_to_count_is_legal_and_minus_one) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [1, 2, 3]; "
        "  Console.WriteLine(arr.IndexOf(1, arr.Count)); "
        "end Main; end C;"), "-1");
}

// stdlib §4.4: "0 <= startIndex <= Count required, IndexOutOfRangeException
// otherwise" — above Count. Observed through the catch arm's stdout, not an
// exit code, so neither an abort nor the pre-fix silent no-op reads as green:
// the no-raise path prints its own distinct marker.
BEZEL_TEST(array_indexof_startindex_4744, start_above_count_raises) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [1, 2, 3]; "
        "  try "
        "    _ := arr.IndexOf(1, 4); "
        "    Console.WriteLine(\"no-raise\"); "
        "  catch "
        "    IndexOutOfRangeException => Console.WriteLine(\"caught\") "
        "  end; "
        "end Main; end C;"), "caught");
}

// stdlib §4.4: the same guard on the negative side of the closed interval.
BEZEL_TEST(array_indexof_startindex_4744, negative_start_raises) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [1, 2, 3]; "
        "  try "
        "    _ := arr.IndexOf(1, -1); "
        "    Console.WriteLine(\"no-raise\"); "
        "  catch "
        "    IndexOutOfRangeException => Console.WriteLine(\"caught\") "
        "  end; "
        "end Main; end C;"), "caught");
}

// The default still applies: the one-argument spelling is unchanged and keeps
// answering the first occurrence from 0. Guards the shared scan, which the fix
// reuses for both arities.
BEZEL_TEST(array_indexof_startindex_4744, one_arg_form_unchanged) {
    const char* prefix =
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<int> := [7, 7, 7, 9]; "
        "  Console.WriteLine(arr.IndexOf(";
    BEZEL_EXPECT_EQ(runAotOut(std::string(prefix) + "7)); end Main; end C;"), "0");
    BEZEL_EXPECT_EQ(runAotOut(std::string(prefix) + "9)); end Main; end C;"), "3");
    BEZEL_EXPECT_EQ(runAotOut(std::string(prefix) + "42)); end Main; end C;"), "-1");
}

// §4.4 with a string element type — the scan dispatches element equality
// through emitRemoveElementEquality (§4.6: strings compare by content), so the
// start offset must compose with that tail, not only with the scalar one.
BEZEL_TEST(array_indexof_startindex_4744, string_elements_with_start) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr: Array<string> := [\"a\", \"b\", \"a\", \"c\"]; "
        "  Console.WriteLine(arr.IndexOf(\"a\", 1)); "
        "end Main; end C;"), "2");
}
