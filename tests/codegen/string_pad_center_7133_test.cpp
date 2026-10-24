// Regression tests for #7133 — stdlib §9.6 DECLARES `string.PadCenter` with a
// full contract, but the compiler had NO implementation of it.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343) declares the member:
//         public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//     with the comment: "Pad contract (all three Pad members): totalWidth <=
//     Length returns the original string unchanged; totalWidth < 0 raises
//     ArgumentOutOfRangeException. PadCenter splits the padding evenly; when
//     it is odd, the extra character goes to the RIGHT: \"ab\".PadCenter(5) =
//     \" ab  \"."
//   * language spec §4.6 (v0.343): a `string` is runtime-managed and its
//     code points are stored at a FIXED width (1/2/4 bytes per code point).
//   * language spec §28.9 (v0.343): runtime-managed values are reference
//     counted; a fresh runtime-managed string hands its consumer a +1
//     reference that the consumer must release exactly once.
//
// THE DEFECT. `PadCenter` was absent from the lowering: the ONLY mention of
// the name in `src/codegen/LlvmBackend_String.cpp` was a COMMENT, and the
// string instance-member ACCEPT SET in `src/sema/ExprTypecheck_Calls.cpp`
// (`resolveMemberAccess`) did not list it either, so the catch-all reject
// fired. MEASURED on the pre-fix compiler (base 6561d18b):
//
//     Console.WriteLine("[" + "ab".PadCenter(5) + "]");
//
// ⇒ REJECTED: "string member `.PadCenter` is not yet supported in codegen"
//   (rc = 1, no program produced). The whole §9.6 PadCenter contract was
//   unreachable.
//
// THE FIX. `PadCenter` joins the SAME arm as `PadLeft`/`PadRight`:
//   * the sema instance-member accept set gains `"PadCenter"` (one line);
//   * the codegen arm gains `"PadCenter"` and passes the runtime helper
//     `bz_str_pad`'s alignment byte `2` (the helper's `align` byte selects
//     0 = PadLeft / 1 = PadRight / 2 = PadCenter — the centered layout splits
//     the padding evenly with the ODD extra code point on the RIGHT);
//   * the `isPlusOneStringProducer` recognizer gains `"PadCenter"` — the
//     result is a fresh runtime-managed string (rc := 1), so a consumer must
//     TRANSFER that reference instead of retaining on top of it;
//   * a `totalWidth < 0` guard raises `ArgumentOutOfRangeException` BEFORE the
//     runtime call, because the helper answers `totalWidth <= Length` with
//     "the original string unchanged" and would otherwise swallow a negative
//     width.
//
// ⚠ The §5.1 TYPE GATE IS NOT PART OF THIS FIX. `checkBuiltinStringCallArgs`'
// `intPos` set (src/sema/ExprTypecheck_Calls.cpp) already named `"PadCenter"`
// at position 0 and already mapped its position-1 `paddingChar` to `char`, so
// the gate EXISTED before this change and is left untouched. MEASURED before
// and after: `"abc".PadCenter(b)` with `b: uint` is REJECTED with the §5.1
// implicit-convertibility diagnostic ("between signed and unsigned types of
// the same or narrower width is never implicit"). Row (H) below pins that the
// diagnostic SURVIVES the fix.
//
// ORACLE CHOICE. The pre-fix symptom is a REJECTION (rc = 1, no program), so
// the value rows assert the OUTPUT — a row that merely asserted "did not
// crash" would be vacuous here. The leak row uses `runAotChecked` (LSan
// armed) because the fix mints a runtime-managed string: without the
// recognizer arm the producer's +1 would never be released.
//
// ⚠ The negative-width contract of the SIBLING members `PadLeft`/`PadRight`
// (they answer a negative totalWidth with the receiver unchanged) is a
// SEPARATE defect, tracked by #7120, and is NOT asserted here.

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

// Build a "render N times, count byte-correct results" program. `decl` binds
// the receiver(s); `render` is the expression under test; `expectedLen` is the
// exact length the render must have. Returns 40 iff every render matched AND
// the run is leak-clean.
std::string loopCount(const char* decl, const char* render, int expectedLen) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           decl +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 40 do "
           "      var s := " + render + "; "
           "      if s.Length = " + std::to_string(expectedLen) +
               " then count := count + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// (A) The #7133 repro, exactly: the odd-padding split, with the extra
// character on the RIGHT.
//
// stdlib §9.6 (v0.343): "PadCenter splits the padding evenly; when it is odd,
// the extra character goes to the RIGHT: \"ab\".PadCenter(5) = \" ab  \"."
// ⇒ one pad character on the left, TWO on the right.
//
// Pre-fix: REJECTED ("string member `.PadCenter` is not yet supported in
// codegen"), rc = 1, no program.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           odd_padding_extra_character_goes_to_the_right_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[ ab  ]\n");
}

// ===========================================================================
// (B) The EVEN split — the two sides must be equal, which distinguishes
// "split evenly" from "put everything on one side".
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           even_padding_is_split_evenly_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(6) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[  ab  ]\n");
}

// ===========================================================================
// (C) The `totalWidth <= Length` half of the Pad contract: the ORIGINAL
// string is returned unchanged (not padded, not truncated).
//
// stdlib §9.6 (v0.343): "totalWidth <= Length returns the original string
// unchanged".
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           totalwidth_at_or_below_length_returns_the_original_unchanged_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"abc\".PadCenter(3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadCenter(2) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abc\".PadCenter(0) + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[abc]\n[abc]\n[abc]\n");
}

// ===========================================================================
// (D) The `totalWidth < 0` half of the Pad contract: ArgumentOutOfRangeException.
//
// stdlib §9.6 (v0.343): "totalWidth < 0 raises ArgumentOutOfRangeException".
// The row asserts the EXCEPTION (the catch arm's exit code 42), not merely
// that the program did not crash.
//
// ⚠ The row must also distinguish "the exception was raised" from "the
// program returned the receiver unchanged" (the pre-existing behaviour of the
// sibling `PadLeft`/`PadRight`, and what the runtime helper would answer if
// the guard were not emitted): the catch arm returns 42 and the non-throwing
// arm returns 0, so the two are distinguishable by the exit code.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           negative_totalwidth_raises_argument_out_of_range_stdlib_9_6) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      var s := \"ab\".PadCenter(-1);\n"
        "      Console.WriteLine(\"[\" + s + \"]\");\n"
        "      return 0;\n"
        "    catch ArgumentOutOfRangeException => return 42;\n"
        "    end;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The exception is catchable as its BASE type too (stdlib §2.5:
// ArgumentOutOfRangeException derives from ArgumentException) — the guard
// raises the DERIVED type, so a base-typed catch must find it.
BEZEL_TEST(string_pad_center_7133,
           negative_totalwidth_is_catchable_as_argument_exception_stdlib_2_5) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      var s := \"ab\".PadCenter(-3);\n"
        "      return 0;\n"
        "    catch ArgumentException => return 43;\n"
        "    end;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// (E) The DEFAULT `paddingChar` is a SPACE (stdlib §9.6:
// `paddingChar: char := ' '`), and the EXPLICIT spelling overrides it.
//
// The default row prints the same program twice — once with the default and
// once with an explicit ' ' — so the two must agree byte for byte.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           default_padding_char_is_a_space_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5, ' ') + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[ ab  ]\n[ ab  ]\n");
}

// The EXPLICIT padding character — the same split, a different character.
BEZEL_TEST(string_pad_center_7133,
           explicit_padding_char_is_used_stdlib_9_6) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(5, '*') + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadCenter(7, '-') + \"]\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[*ab**]\n[--ab---]\n");
}

// ===========================================================================
// (F) §4.6 — the padding is CODE-POINT aware, not byte aware. A receiver
// holding a code point above U+00FF (stored at width 2, §4.6) must be padded
// to a WIDTH in code points, and the wide code point must survive intact.
//
// The program compares the padded result against the EXPECTED string built in
// the SAME program, so the assertion does not depend on how the terminal
// encodes the wide code point: the exit code is 0 iff the two strings are
// equal, 1 otherwise. A byte-wise padding (which would truncate or
// mis-measure) makes them differ.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           wide_code_points_are_padded_by_code_point_count_spec_4_6) {
    JitResult r = runAot(prog(
        "    var s := \"a\\u4e2db\".PadCenter(5);\n"
        "    var expected := \" a\\u4e2db \";\n"
        "    Console.WriteLine(s.Length);\n"
        "    return (if s = expected then 0 else 1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The same claim for a Latin-1 code point (stored at width 1, §4.6): the
// padded result must still be 5 CODE POINTS.
BEZEL_TEST(string_pad_center_7133,
           latin1_code_points_are_padded_by_code_point_count_spec_4_6) {
    JitResult r = runAot(prog(
        "    var s := \"a\\u00e9b\".PadCenter(5);\n"
        "    var expected := \" a\\u00e9b \";\n"
        "    Console.WriteLine(s.Length);\n"
        "    return (if s = expected then 0 else 1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (G) LEAK-FREEDOM — the §28.9 / §4.6 producer reference.
//
// `bz_str_pad` ends in `bz_string_init_rc` (rc := 1): the result is a FRESH
// runtime-managed string, so a consumer must TRANSFER that reference. The
// `isPlusOneStringProducer` recognizer is what tells the consumer so; without
// the `PadCenter` arm the consumer treats the result as a BORROW, retains it
// (rc 1 -> 2) and the producer's original +1 is never dropped — the leak
// #7119 measured for `Repeat`/`GetField`.
//
// The row renders in a LOOP (40 iterations) so LSan RELIABLY reports the leak
// pre-fix, and the returned count == the iteration count IFF every render is
// byte-correct AND the run is leak-clean.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           padcenter_result_is_leak_free_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "\"ab\".PadCenter(5)", 5));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // LSan-clean: the producer's +1 released
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// The bound-local consumer shape: the result is stored in a named local and
// released at scope end. Without the recognizer arm the local's release
// drops one reference and the producer's +1 is left dangling (a leak).
BEZEL_TEST(string_pad_center_7133,
           padcenter_bound_local_is_leak_free_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "var src := \"ab\"; ",
        "src.PadCenter(7)", 7));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// The CONTROL arm — the already-recognized sibling `PadLeft` (same helper,
// same minting path) must stay clean: the widened recognizer must not have
// introduced a double-release.
BEZEL_TEST(string_pad_center_7133,
           padleft_control_is_leak_free_stdlib_9_6) {
    JitResult r = runAotChecked(loopCount(
        "",
        "\"ab\".PadLeft(5)", 5));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// (H) The §5.1 TYPE GATE — it EXISTED before this fix (`checkBuiltinStringCallArgs`'
// `intPos` set already named `"PadCenter"` at position 0) and must SURVIVE it.
//
// stdlib §9.6 (v0.343) declares `totalWidth: int`. §5.1: "between signed and
// unsigned types of the same or narrower width is never implicit" ⇒ a `uint`
// totalWidth is rejected.
//
// The row asserts the SPECIFIC diagnostic, so a REJECTION of a different kind
// (or a crash) does not pass it.
// ===========================================================================
BEZEL_TEST(string_pad_center_7133,
           uint_totalwidth_is_rejected_by_the_section_5_1_gate_spec_5_1) {
    JitResult r = runAot(prog(
        "    var b: uint := 5;\n"
        "    var s := \"ab\".PadCenter(b);\n"
        "    Console.WriteLine(s);\n"));
    // The rejection must be a REAL compile-time rejection (not a crash, not a
    // timeout, not an ASan report) — `ran == false` alone cannot tell those
    // apart (see tests/support/aot_result_asserts.hpp).
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    // …and it must be the §5.1 implicit-convertibility diagnostic naming
    // `string.PadCenter` — not some other rejection.
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "string.PadCenter"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "5.1"));
}
