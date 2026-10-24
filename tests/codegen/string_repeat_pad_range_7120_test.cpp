// Regression tests for #7120 — stdlib §9.6 declares a RANGE CONTRACT for
// `Repeat`'s count and for the `Pad*` members' `totalWidth`, and the compiler
// enforces neither. Worse, the two `Repeat` spellings DISAGREE with each
// other about a negative count.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343) declares the STATIC count contract:
//         public static pure method Repeat(c: char, count: int): string;
//         // count < 0 raises ArgumentOutOfRangeException; count = 0 is ""
//         public static pure method Repeat(unit: string, count: int): string;
//         // same count contract; Repeat("", n) = ""
//     — the SAME contract on the `unit: string` overload ("same count
//     contract"), i.e. the count contract is the contract of REPEATING A
//     UNIT `count` TIMES, whatever the unit is.
//   * stdlib §9.6 (v0.343) declares the Pad contract for all three Pad
//     members:
//         public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
//         public pure method PadRight(totalWidth: int, paddingChar: char := ' '): string;
//         public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//         // Pad contract (all three Pad members): totalWidth <= Length returns
//         // the original string unchanged; totalWidth < 0 raises
//         // ArgumentOutOfRangeException.
//   * language spec §5.1 (v0.343): implicit numeric conversions lose no
//     programmer-visible information — `byte → int` is a LOSSLESS widening, so
//     a `byte` 200 reaches an `int` parameter as the UNSIGNED value 200.
//   * language spec §4.6 (v0.343): a `string`'s code points are stored at a
//     FIXED width, so a length is a count of CODE POINTS.
//   * language spec §28.9 (v0.343): runtime-managed values are reference
//     counted; a fresh runtime-managed string hands its consumer a +1
//     reference that the consumer must release exactly once.
//
// THE DEFECT (MEASURED on the pre-fix compiler, base 3c2ef83e).
//
//   * `"x".Repeat(-1).Length` — the INSTANCE spelling had NO guard on the
//     count. A negative count made the total length negative, `malloc`
//     returned a 1-byte buffer and the copy loop wrote past it:
//       ASan: "heap-buffer-overflow ... WRITE of size 1"; without a sanitizer
//       the process dies (SIGSEGV, rc = 139).
//   * `string.Repeat('x', -1).Length` — the STATIC spelling CLAMPED a
//     negative count to 0 and printed `0` (the empty string), exiting 0.
//   * `"abc".PadLeft(-1).Length` — printed `3` (the receiver, unchanged).
//
// ⇒ The SAME method's two spellings disagreed: one corrupted memory, the
// other silently returned "". §9.6 says both must RAISE
// ArgumentOutOfRangeException.
//
// THE FIX.
//
//   * the instance `Repeat` arm (`src/codegen/LlvmBackend_String.cpp`) gains
//     the count guard, emitted BEFORE the `malloc`;
//   * the static `Repeat` arm (`src/codegen/LlvmBackend_Calls2.cpp`) REPLACES
//     its clamp with that same guard, and widens the count by the SOURCE's
//     signedness (§5.1) — the widening is a PREREQUISITE of the guard, which
//     must test the count's §5.1 value;
//   * the `Pad*` arm gains the guard for `PadLeft`/`PadRight` beside the one
//     `PadCenter` already had (all three now share ONE guard);
//   * a guard that raises releases the caller's +1 producer receiver on the
//     throw edge (§28.9 — the exception unwinds past the caller's post-call
//     release), so the raise does not leak.
//
// ORACLE CHOICE. Every row asserts the RUNTIME outcome (the catch arm's exit
// code), because §9.6's contract is a runtime raise — a row that merely
// asserted "did not crash" would be vacuous (the pre-fix program crashes for
// the instance spelling, and silently succeeds for the other three). The
// memory-safety rows use `runAotChecked` (ASan + LSan armed) because the
// pre-fix symptom is an out-of-bounds WRITE and the raise must not leak.
//
// ⚠ The conformance corpus runner arms NO sanitizer, so a corpus row could
// not assert either the absence of the out-of-bounds write or the
// leak-freedom of the raise ⇒ those rows live HERE, not in the corpus.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Wraps `body` in a runnable `Main` that returns 0.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A program whose single statement is `render`, wrapped in a try/catch that
// returns 42 when an ArgumentOutOfRangeException is raised and 0 otherwise.
// The two are distinguishable by the exit code, so the row asserts the
// EXCEPTION rather than merely "the program did not crash".
std::string raising(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var s := " + render + ";\n"
           "      Console.WriteLine(s.Length);\n"
           "      return 0;\n"
           "    catch ArgumentOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but catching the BASE type (stdlib §2.5:
// ArgumentOutOfRangeException derives from ArgumentException) — the guard
// raises the DERIVED type, so a base-typed catch must find it.
std::string raisingAsBase(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var s := " + render + ";\n"
           "      Console.WriteLine(s.Length);\n"
           "      return 0;\n"
           "    catch ArgumentException => return 43;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// A program that renders `render` in a LOOP of 40 iterations, each inside its
// own try/catch, and returns the number of iterations that raised. The loop
// makes LSan RELIABLY report a leak; the returned count is 40 iff EVERY
// iteration raised.
std::string loopRaising(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var count := 0;\n"
           "    var i := 0;\n"
           "    while i < 40 do\n"
           "      try\n"
           "        var s := " + render + ";\n"
           "        count := count + 100;\n"
           "      catch ArgumentOutOfRangeException => count := count + 1;\n"
           "      end;\n"
           "      i := i + 1;\n"
           "    end;\n"
           "    return count;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// (A) The INSTANCE spelling — the issue's literal repro.
//
// stdlib §9.6 (v0.343): "count < 0 raises ArgumentOutOfRangeException".
//
// Pre-fix: the instance arm had NO guard; the count made the total length
// negative, `malloc` returned a 1-byte buffer and the copy loop wrote past
// it (ASan: heap-buffer-overflow, WRITE of size 1; without a sanitizer:
// SIGSEGV, rc = 139).
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           instance_repeat_negative_count_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("\"x\".Repeat(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The SAME contract on a MULTI-CODE-POINT receiver (§4.6 — the length is a
// count of code points, so the receiver's width must not change the
// contract).
//
// Pre-fix: the total length was -10, `malloc` failed and the NUL terminator
// was written through the null pointer (SIGSEGV, rc = 139).
BEZEL_TEST(string_repeat_pad_range_7120,
           instance_repeat_negative_count_on_a_multi_code_point_receiver_raises_stdlib_9_6) {
    JitResult r = runAot(raising("\"abcdefghij\".Repeat(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (B) The STATIC spelling — the SAME §9.6 count contract.
//
// Pre-fix: the static arm CLAMPED a negative count to 0 and printed `0` (the
// empty string), exiting 0 — the silent-wrong-answer half of the issue.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           static_repeat_negative_count_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("string.Repeat('x', -1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (C) The two spellings must AGREE. Each row above pins one spelling; this
// row pins the agreement directly: both spellings must raise the SAME
// exception type, so a base-typed catch must find both.
//
// stdlib §2.5: ArgumentOutOfRangeException derives from ArgumentException.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           negative_count_is_catchable_as_argument_exception_in_both_spellings_stdlib_2_5) {
    JitResult r = runAot(raisingAsBase("\"x\".Repeat(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 43);

    JitResult r2 = runAot(raisingAsBase("string.Repeat('x', -1)"));
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_FALSE(r2.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r2.exitCode, 43);
}

// ===========================================================================
// (D) The Pad contract — "totalWidth < 0 raises
// ArgumentOutOfRangeException", for ALL THREE Pad members.
//
// Pre-fix: `PadLeft`/`PadRight` answered a negative width with the receiver
// unchanged (the runtime helper treats `totalWidth <= Length` as "no padding
// needed"), so they printed the receiver's length and exited 0.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           padleft_negative_totalwidth_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("\"abc\".PadLeft(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(string_repeat_pad_range_7120,
           padright_negative_totalwidth_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("\"abc\".PadRight(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// `PadCenter` already raised (the #7133 pick added its guard) ⇒ this row
// pins that the #7120 change did NOT weaken it: the three Pad members now
// share ONE guard, and `PadCenter` keeps raising.
BEZEL_TEST(string_repeat_pad_range_7120,
           padcenter_negative_totalwidth_still_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(raising("\"abc\".PadCenter(-1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (E) The OTHER half of the count contract: `count = 0` is NOT an error.
//
// stdlib §9.6 (v0.343): "count < 0 raises ArgumentOutOfRangeException;
// count = 0 is \"\"".
//
// This row is the CONTROL for the guards above: a guard that rejected every
// count ≤ 0 (or clamped a negative one) would break it.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           repeat_zero_count_is_the_empty_string_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"x\".Repeat(0) + \"]\");\n"
        "    Console.WriteLine(\"[\" + string.Repeat('x', 0) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"xy\".Repeat(0) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[]\n[]\n[]\n");
}

// A POSITIVE count still repeats the receiver — and the two spellings agree
// on the result (§9.6's "unit" contract: `Repeat(unit, count)` is a
// `count`-long string of the unit).
BEZEL_TEST(string_repeat_pad_range_7120,
           repeat_positive_count_repeats_the_receiver_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"x\".Repeat(3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"xy\".Repeat(3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + string.Repeat('x', 3) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[xxx]\n[xyxyxy]\n[xxx]\n");
}

// ===========================================================================
// (F) The Pad contract's other half — "totalWidth <= Length returns the
// original string unchanged" — and the valid widths, which must be UNCHANGED
// by the new guard.
//
// The row prints the widths on both sides of the receiver's length, so a
// guard that rejected a too-small width (or a width equal to the length)
// would be caught.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           pad_valid_widths_are_unchanged_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"abc\".PadLeft(0) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadLeft(3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadLeft(5) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadRight(5) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadCenter(5) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[abc]\n[abc]\n[  abc]\n[abc  ]\n[ abc ]\n");
}

// ===========================================================================
// (G) MEMORY SAFETY — the instance `Repeat` on the `runAotChecked` oracle
// (ASan + LSan armed).
//
// Pre-fix: the negative count produced a 1-byte `malloc` and the copy loop
// wrote past it ⇒ ASan "heap-buffer-overflow ... WRITE of size 1". The row
// is on the CHECKED oracle because the pre-fix symptom is exactly what a
// sanitizer detects and an unsanitized run only reports as SIGSEGV.
//
// Post-fix: the count guard raises BEFORE the `malloc`, so nothing is
// allocated and the catch arm's exit code is 42.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           instance_repeat_negative_count_is_memory_safe_stdlib_9_6) {
    JitResult r = runAotChecked(raising("\"x\".Repeat(-1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no ASan report, no LSan leak
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The same shape on a PRODUCER receiver (§28.9): the receiver `("a" + "b")`
// is a +1 string producer, and the caller releases it AFTER the call
// returns — a raise unwinds past that release, so the guard must release the
// receiver on the throw edge. The row renders in a LOOP (40 iterations) so
// LSan RELIABLY reports a leak if the release is missing, and the returned
// count is 40 iff EVERY iteration raised.
//
// Pre-fix: `PadLeft` did not raise at all (count += 100 per iteration ⇒ the
// program returned 4000).
BEZEL_TEST(string_repeat_pad_range_7120,
           pad_raise_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(loopRaising("(\"a\" + \"b\").PadLeft(-1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean: the producer's +1 released
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// The same shape for the instance `Repeat` raise.
BEZEL_TEST(string_repeat_pad_range_7120,
           instance_repeat_raise_does_not_leak_the_producer_receiver_stdlib_9_6) {
    JitResult r = runAotChecked(loopRaising("(\"a\" + \"b\").Repeat(-1)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (H) §5.1 — the count reaches the `int` parameter as its §5.1 VALUE, and
// the two spellings must agree on it.
//
// The static arm used to widen the count with a bare `CreateSExt` and then
// CLAMP the result to 0, so a `byte` 200 arrived as -56 and was reported as
// the EMPTY string (printed `0`) while the instance spelling printed `200`.
// The range guard must test the count's §5.1 value (`byte → int` is a
// LOSSLESS widening ⇒ 200), so the count is now widened by the SOURCE's
// signedness and ONE value is used both by the guard and as the length.
//
// ⚠ This row pins a BEHAVIOUR CHANGE of the static spelling: `byte` 200
// used to print `0`, and now prints `200` (the #7117 symptom, repaired by the
// widening the guard requires).
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           byte_count_reaches_the_int_parameter_unsigned_in_both_spellings_stdlib_5_1) {
    JitResult r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(string.Repeat('x', b).Length);\n"
        "    Console.WriteLine(\"x\".Repeat(b).Length);\n"
        "    Console.WriteLine(string.Repeat('x', 200).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n200\n");
}

// ===========================================================================
// (I) The `ushort` SIBLING of row (H) — the static spelling's value.
//
// Row (H) pins the `byte` count. The `ushort` sibling is the SAME rule with
// a wider source type, and it was the case the #7120 REVIEWER measured and
// the #7120 commit body did NOT name: the static spelling's count was
// widened with a bare `CreateSExt` and CLAMPED, so a `ushort` 65535 arrived
// as -1 and the count was reported as the EMPTY string (printed `0`).
//
// ⚠ This row pins a BEHAVIOUR CHANGE of the STATIC spelling: `ushort` 65535
// used to print `0`, and now prints `65535` — the same source-signedness
// widening as row (H), one type wider.
//
// §5.1 (v0.343): `ushort → int` is a LOSSLESS widening ("Widening from
// unsigned to a strictly larger signed type (e.g. `byte → short`,
// `ushort → int`) is permitted because no value is lost"), so the count
// reaches the `int` parameter as 65535.
//
// FAIL-WITHOUT (MEASURED, on the pre-#7120 compiler `bezelc.base`, the
// #7120 reviewer's own base build, verified by re-running row (H)'s `byte`
// case on it and getting `0` where the fix gives `200`):
//
//   pre-#7120:  `string.Repeat('x', u)` printed 0   ("x".Repeat(u) printed
//               65535 — the INSTANCE arm was already right)
//   after:      `string.Repeat('x', u)` printed 65535
//
// ⇒ this row FAILS on the base (the base prints `0`, the row expects
// `65535`) and PASSES with the fix.
//
// The row NAME states the VALUE (65535) rather than the spelling's
// signedness, so it cannot be misread as a claim about the parameter's
// signedness — the parameter is a SIGNED `int` (§9.6's
// `Repeat(c: char, count: int)`); it is the SOURCE's value that is
// unsigned.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           ushort_count_reaches_the_int_parameter_as_65535_static_spelling_stdlib_5_1) {
    JitResult r = runAot(prog(
        "    var u: ushort := 65535;\n"
        "    Console.WriteLine(string.Repeat('x', u).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "65535\n");
}

// ===========================================================================
// (J) The CONTROL for row (I) — the INSTANCE spelling, which was already
// right on the base and must stay right.
//
// The pre-#7120 compiler printed `65535` here (the instance arm used the
// count directly), so this row PASSES on BOTH arms. It is the control that
// shows row (I)'s flip is the STATIC spelling's and not the rule's: if this
// row ever fails, the source-signedness widening has reached the instance
// arm too, and the two spellings have stopped agreeing.
// ===========================================================================
BEZEL_TEST(string_repeat_pad_range_7120,
           ushort_count_reaches_the_int_parameter_as_65535_instance_spelling_stdlib_5_1) {
    JitResult r = runAot(prog(
        "    var u: ushort := 65535;\n"
        "    Console.WriteLine(\"x\".Repeat(u).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "65535\n");
}
