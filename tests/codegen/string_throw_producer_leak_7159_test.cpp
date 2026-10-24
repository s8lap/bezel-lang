// Regression tests for #7159 — a §9.6 string method that RAISES leaves the
// PRODUCER RECEIVER unreleased, so the caller's post-call release is
// unreachable and the producer's +1 reference leaks.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343) declares the string members whose position / count
//     contracts can fail:
//         public pure method IndexOf(value: char, startIndex: int := 0): int;
//         public pure method IndexOf(value: string, startIndex: int := 0): int;
//         //   0 <= startIndex <= Length (IndexOutOfRangeException otherwise)
//         public pure method Substring(startIndex: int): string;
//         public pure method Substring(startIndex: int, length: int): string;
//         public pure method AsSpan(start: int): ReadOnlyStringSpan;
//         public pure method AsSpan(start: int, length: int): ReadOnlyStringSpan;
//         public pure method AsSpan(r: Range<int>): ReadOnlyStringSpan;
//   * language spec §28.9 (v0.343): a fresh runtime-managed string hands its
//     consumer a +1 reference that the consumer must release EXACTLY ONCE.
//   * language spec §31.4 (v0.343): the built-in exception types are
//     trivial-dispose — a raise does not run any user cleanup, so nothing but
//     the compiler-inserted release can free the receiver.
//
// THE DEFECT (MEASURED on the base tree 3983c8ce9c, ASan/LSan).
//
// The caller (src/codegen/LlvmBackend_Exprs.cpp) pre-lowers a +1 producer
// receiver and releases it AFTER the method call returns. A raise branches to
// the unwind block, so that post-call release is UNREACHABLE: the producer's
// reference leaks.
//
// MEASURED, one raise per row, `build/bezelc --sanitize=address`:
//
//   ("a" + "b").IndexOf("z", 999)   -> 35 B in 2 allocations (LEAK)
//   ("a" + "b").IndexOf('z', 999)   -> 35 B in 2 allocations (LEAK)
//   ("a" + "b").Substring(999)      -> 35 B in 2 allocations (LEAK)
//   ("a" + "b").Substring(0, 999)   -> 35 B in 2 allocations (LEAK)
//   ("a" + "b").AsSpan(999)         -> 35 B in 2 allocations (LEAK)
//   ("a" + "b").ToBoolean()         -> 35 B in 2 allocations (LEAK)
//
// Each program CATCHES the exception and returns, so this is NOT an at-exit
// reachability artefact: the exception was caught, the frame unwound, and the
// producer was still never released.
//
// THE FIX — the single funnel `emitGuardThrowReleasingString`.
//
// The #7120 lane fixed three arms (the instance `Repeat`, `PadLeft`/
// `PadRight`/`PadCenter`) by releasing the receiver on the throw edge (the
// #6979 idiom) and left the OTHER raising arms on a plain `emitGuardThrow`,
// which does not release it. The leak is the CLASS, not the arm, so the fix is
// ONE place: every raise inside `tryLowerStringMethod` now goes through
// `emitGuardThrowReleasingString`, which releases the caller-owned +1 producer
// reference on the throw edge before the raise.
//
// ORACLE CHOICE. The leak rows are on `runAotChecked` (ASan + LSan armed)
// because the symptom IS the leak: LSan makes the program's exit code the
// ASan exit code (1) instead of the loop's own count, so the row's expected
// exit code is a positive assertion of leak-freedom. The conformance corpus
// runner arms NO sanitizer, so a corpus row could assert neither the
// leak-freedom nor the raise ⇒ those rows live HERE, not in the corpus.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program that renders `render` in a LOOP of 40 iterations, each inside its
// own try/catch, and returns the number of iterations that raised. The loop
// makes LSan RELIABLY report a leak; the returned count is 40 iff EVERY
// iteration raised AND no leak was reported (a leak makes the ASan runtime
// replace the exit code with its own).
std::string loopRaising(const std::string& render, const std::string& exc) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var count := 0;\n"
           "    var i := 0;\n"
           "    while i < 40 do\n"
           "      try\n"
           "        var s := " + render + ";\n"
           "        count := count + 100;\n"
           "      catch " + exc + " => count := count + 1;\n"
           "      end;\n"
           "      i := i + 1;\n"
           "    end;\n"
           "    return count;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but the arm does NOT raise — the CONTROL for the rows
// above: a valid call in a loop must neither raise nor leak. A release
// emitted twice (or on the normal path when the receiver is not the caller's)
// shows up here as a crash (a use-after-free under ASan) or a wrong count.
std::string loopNotRaising(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var count := 0;\n"
           "    var i := 0;\n"
           "    while i < 40 do\n"
           "      try\n"
           "        var s := " + render + ";\n"
           "        count := count + 1;\n"
           "      catch Exception => count := count + 100;\n"
           "      end;\n"
           "      i := i + 1;\n"
           "    end;\n"
           "    return count;\n"
           "  end Main;\n"
           "end C;\n";
}

// A single raise, caught, with the catch arm's exit code as the only
// observable — used to pin the EXCEPTION TYPE (a raise of a different type
// would not be caught here, and the program would exit with the
// uncaught-exception code instead).
std::string raisingOnce(const std::string& render, const std::string& exc) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var s := " + render + ";\n"
           "      Console.WriteLine(s);\n"
           "      return 0;\n"
           "    catch " + exc + " => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// (A) `IndexOf(value: string, startIndex)` — §9.6 (v0.343): "0 <= startIndex
// <= Length (IndexOutOfRangeException otherwise)".
//
// Pre-fix: the raise leaked the producer receiver (35 B in 2 allocations).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           indexof_string_start_index_out_of_range_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").IndexOf(\"z\", 999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean: the producer's +1 released
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// The same raise, once, to pin the EXCEPTION TYPE: the catch arm's exit code
// (42) is reached only if the raise is an IndexOutOfRangeException.
BEZEL_TEST(string_throw_producer_leak_7159,
           indexof_string_start_index_out_of_range_raises_index_out_of_range_stdlib_9_6) {
    JitResult r = runAot(
        raisingOnce("(\"a\" + \"b\").IndexOf(\"z\", 999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (B) `IndexOf(value: char, startIndex)` — the CHAR-needle arm of the same
// §9.6 contract (the comment names the bound for the pair).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           indexof_char_start_index_out_of_range_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").IndexOf('z', 999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (C) `Substring(startIndex)` — the sibling position member.
//
// Pre-fix: the raise leaked the producer receiver (35 B in 2 allocations).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           substring_out_of_range_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").Substring(999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (D) `Substring(startIndex, length)` — the WINDOW arm: the length is out of
// range, the start is not.
//
// Pre-fix: the raise leaked the producer receiver (35 B in 2 allocations).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           substring_length_out_of_range_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").Substring(0, 999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (E) `AsSpan(start)` — the span view's position arm.
//
// Pre-fix: the raise leaked the producer receiver (35 B in 2 allocations).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           asspan_start_out_of_range_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").AsSpan(999)", "IndexOutOfRangeException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (F) `ToBoolean()` — the same CLASS, a different reason to raise: the
// receiver is not "True"/"false", so the member raises FormatException
// (stdlib §2.3's contract). The receiver's producer reference leaks exactly
// the same way.
//
// Pre-fix: the raise leaked the producer receiver (35 B in 2 allocations).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           toboolean_invalid_value_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(
        loopRaising("(\"a\" + \"b\").ToBoolean()", "FormatException"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (G) THE CONTROL — the SAME members with VALID arguments must neither
// raise nor leak, and must not release the receiver twice.
//
// This row is green on BOTH arms (pre-fix and post-fix): it is the shape that
// would break if the throw-edge release were emitted on the normal path too,
// or if the receiver were released when it is not the caller's to release.
// The expected count is 40 (every iteration completed without raising).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           valid_calls_still_complete_without_raising_or_leaking_stdlib_9_6) {
    JitResult r = runAotChecked(loopNotRaising(
        "(\"a\" + \"b\").IndexOf(\"a\", 0).ToString() + "
        "(\"a\" + \"b\").Substring(1) + "
        "(\"a\" + \"b\").AsSpan(1).Length.ToString()"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (H) THE CONTROL — a receiver that is NOT a producer (a named local) must
// not be released by the throw edge: the local owns its reference, and the
// scope-end release is the only one. A release of the local here would
// over-release it (a use-after-free under ASan).
// ===========================================================================
BEZEL_TEST(string_throw_producer_leak_7159,
           a_named_local_receiver_is_not_released_by_the_throw_edge_stdlib_9_6) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var count := 0;\n"
        "    var i := 0;\n"
        "    while i < 40 do\n"
        "      var s := \"a\" + \"b\";\n"
        "      try\n"
        "        var t := s.Substring(999);\n"
        "        count := count + 100;\n"
        "      catch IndexOutOfRangeException => count := count + 1;\n"
        "      end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    return count;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}
