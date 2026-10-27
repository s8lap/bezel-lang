// Regression tests for #7168 — `string.Remove(startIndex)` does not range-check
// `startIndex`, so a program the compiler ACCEPTS reads past the end of a heap
// allocation.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343) declares the member with a POSITION parameter:
//         public pure method Remove(startIndex: int): string;
//         public pure method Remove(startIndex: int, length: int): string;
//     — a window parameter pair, `startIndex` being "the string family's
//     established spelling of the start position" (stdlib §1's "Window
//     parameters" convention).
//   * language spec §4.6 (v0.343): "`s[i]` returns the `char` at code-point
//     position `i` … Out-of-range raises `IndexOutOfRangeException`"; §4.6 also
//     makes a string's length a count of CODE POINTS.
//   * language spec §31.4 (v0.343): "IndexOutOfRangeException — array or
//     string index out of bounds."
//   * stdlib §9.6 (v0.343), `IndexOf`'s own contract: "0 <= startIndex <=
//     Length (IndexOutOfRangeException otherwise)" — the family's position
//     convention.
//   * stdlib §3.1 (v0.343), the `StringBuilder` twin: "`Insert` and `Remove`
//     raise `IndexOutOfRangeException` when the index or the `(startIndex,
//     length)` window falls outside `0..Length`."
//
// THE DEFECT (MEASURED on the base tree 3983c8ce9c).
//
//   var s := ("a" + "b");
//   var t := s.Remove(999);
//
// ⇒ the compiler ACCEPTS it (rc 0) and the program reads 999 bytes past a
// 3-byte allocation:
//
//   ERROR: AddressSanitizer: heap-buffer-overflow on address 0x7be3ab9e0013
//   READ of size 999 at 0x7be3ab9e0013 thread T0
//       #0 ... in memcpy
//   0x7be3ab9e0013 is located 0 bytes after 3-byte region [0x7be3ab9e0010,...)
//   SUMMARY: AddressSanitizer: heap-buffer-overflow ... in memcpy
//
// The 2-arg form has the same defect twice: an out-of-range START reads past
// the end, and a NEGATIVE count makes the suffix's byte offset negative — a
// read BEFORE the allocation.
//
// ⚠ §9.6 (v0.343) states NO range contract for `Remove` — the silence #7108
// records. That silence cannot license an out-of-bounds READ; the guard below
// makes the out-of-range case a defined raise, and the exception type follows
// the family's POSITION convention (IndexOutOfRangeException — the sibling
// `Substring`, the string indexer, `IndexOf`, and §3.1's StringBuilder
// `Remove`), NOT the count/width convention (§9.6's `Repeat` count and `Pad*`
// width raise ArgumentOutOfRangeException — those are NEGATIVE counts, not
// positions).
//
// ORACLE CHOICE. The memory-safety row is on `runAotChecked` (ASan + LSan
// armed) because the pre-fix symptom is exactly what a sanitizer detects; the
// conformance corpus runner arms NO sanitizer, so a corpus row could assert
// neither the absence of the out-of-bounds read nor the raise ⇒ those rows
// live HERE, not in the corpus.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program that calls `render` on the receiver "abcd", inside a try/catch
// that returns 42 when an IndexOutOfRangeException is raised and 0 otherwise.
// The two are distinguishable by the exit code, so the row asserts the
// EXCEPTION rather than merely "the program did not crash".
std::string raising(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s := \"abcd\";\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but the RECEIVER is a +1 string PRODUCER (`("a" + "b")`)
// rather than a literal.
//
// ⚠ This is the shape the MEMORY-SAFETY rows must use: a string LITERAL's
// storage is not a heap allocation ASan bounds-checks, so the out-of-bounds
// READ a literal receiver produces is INVISIBLE to the sanitizer — the row
// would then pin only "the program did not raise". A producer's storage is
// heap-allocated, so the same call is a real ASan
// "heap-buffer-overflow … READ of size 999 … 0 bytes after 3-byte region".
//
// The producer receiver also makes the raise release the producer's +1
// reference on the throw edge (§28.9) — the guard's job, and the #6979 idiom
// the base tree already carries for the `Repeat`/`Pad*` arms.
std::string raisingOnProducer(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but catching the BASE type — the guard raises a
// stdlib §2.5-derived built-in, so a base-typed catch must find it.
std::string raisingAsBase(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s := \"abcd\";\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch Exception => return 43;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// A program that prints the results of the IN-RANGE calls — the CONTROL for
// the guard: the valid shapes must not change.
std::string inRange() {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s := \"abcd\";\n"
           "    Console.WriteLine(\"[\" + s.Remove(1) + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Remove(0) + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Remove(4) + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Remove(1, 2) + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Remove(0, 4) + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Remove(0, 0) + \"]\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// (A) The issue's literal repro — `Remove(startIndex)` with a start past the
// end of the receiver.
//
// Pre-fix: compiles clean, rc 0, and the program reads 999 bytes past a 3-byte
// allocation (ASan: heap-buffer-overflow, READ of size 999).
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_start_index_out_of_range_raises_index_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("s.Remove(999)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The same raise, caught by the BASE type (stdlib §2.5) — the guard raises a
// built-in derived from `Exception`, so a base-typed catch must find it.
BEZEL_TEST(string_remove_range_7168,
           remove_start_index_out_of_range_is_catchable_as_exception_stdlib_2_5) {
    JitResult r = runAot(raisingAsBase("s.Remove(999)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// (B) MEMORY SAFETY — the same call on the `runAotChecked` oracle (ASan +
// LSan armed), on a PRODUCER receiver so the sanitizer really bounds-checks
// the copy (see `raisingOnProducer`).
//
// Pre-fix: "AddressSanitizer: heap-buffer-overflow … READ of size 999 … 0
// bytes after 3-byte region … in memcpy" — the program is aborted by the
// sanitizer, so it never reaches the catch arm.
// Post-fix: the guard raises BEFORE the copy, so nothing is read out of
// bounds; the catch arm's exit code is 42, and the producer's +1 reference
// is released on the throw edge (§28.9), so LSan is quiet too.
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_start_index_out_of_range_is_memory_safe_stdlib_9_6) {
    JitResult r = runAotChecked(raisingOnProducer("(\"a\" + \"b\").Remove(999)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no ASan report, no LSan leak
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The MEMORY-SAFETY row for the 2-arg form's NEGATIVE length — the read
// BEFORE the allocation.
//
// Pre-fix: the negative count made the suffix's byte offset negative, so the
// copy read before the buffer (ASan: "heap-buffer-overflow … READ of size 999
// at 0 bytes before the region"); the program is aborted by the sanitizer.
BEZEL_TEST(string_remove_range_7168,
           remove_negative_length_is_memory_safe_stdlib_9_6) {
    JitResult r = runAotChecked(raisingOnProducer("(\"a\" + \"b\").Remove(0, -1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (C) A NEGATIVE start — the other side of the position's domain.
//
// Pre-fix: the negative start reached `malloc` as a negative allocation size
// and then `memcpy` as a negative copy size (fix #7102's widening note).
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_negative_start_index_raises_index_out_of_range_stdlib_9_6) {
    JitResult r = runAotChecked(raising("s.Remove(-1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (D) The WINDOW — `Remove(startIndex, length)` with a window past the end
// of the receiver (§3.1's StringBuilder `Remove` contract).
//
// Pre-fix: the window's end was CLAMPED to the receiver's length, so the
// program silently removed FEWER code points than it was asked to (and
// reported a shorter string), with no diagnostic.
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_window_out_of_range_raises_index_out_of_range_stdlib_9_6) {
    JitResult r = runAotChecked(raising("s.Remove(0, 999)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (E) A NEGATIVE window length.
//
// Pre-fix: the negative count made the suffix's byte offset NEGATIVE — a read
// BEFORE the allocation (ASan: heap-buffer-overflow, READ of size 999 at
// 0 bytes before the region).
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_negative_length_raises_index_out_of_range_stdlib_9_6) {
    JitResult r = runAotChecked(raising("s.Remove(0, -1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (F) THE CONTROL — the IN-RANGE calls are unchanged.
//
// Green on BOTH arms (pre-fix and post-fix): the guard must not reject a
// valid window. `Remove(4)` on a 4-character string removes NOTHING (start ==
// Length is the empty tail — the same convention `Substring` and the range
// slice use), and `Remove(0, 0)` removes nothing either.
// ===========================================================================
BEZEL_TEST(string_remove_range_7168,
           remove_in_range_results_are_unchanged_stdlib_9_6) {
    JitResult r = runAot(inRange());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[a]\n[]\n[abcd]\n[ad]\n[]\n[abcd]\n");
}
