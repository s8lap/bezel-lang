// Regression tests for issue #7333 — a format spec on a `bool` or `char`
// RECEIVER of the explicit `ToString(format)` member call compiled and
// rendered ("True" / "a") where stdlib §2.18 says it is a COMPILE ERROR.
//
// THE GOVERNING TEXT.
//
// Standard library §2.18 (v0.344), the specifier table's last row:
//
//   | `bool`, `char`, `string` | *(none)* | a format spec on these is a compile error |
//
// so those three types have an EMPTY vocabulary: no specifier can ever apply
// to them. §2.18's neighbouring sentence (the one above the table) says the
// same of every other non-conforming operand: "The primitive and temporal
// types are **intrinsically formattable** (they behave as conforming
// implementations without declaring the interface); **a format spec on any
// other non-conforming operand is a compile error.**"
//
// Language spec §18.5 (v0.344), "Format dispatch": "A hole *with* a format
// spec dispatches to `ToString(format: string)`: the primitive and temporal
// types provide it intrinsically (the specifier vocabulary and its
// invariant-culture semantics are normative in stdlib §2.18) … **A format
// spec on an operand whose type is neither intrinsically formattable nor
// `IFormattable` is a compile error** naming this rule. **An unrecognized
// specifier at runtime raises `FormatException`** (the defined-fault rule,
// stdlib §2.18)."
//
// THE DEFECT. The interpolation hole `$"{b:F4}"` was gated (it dispatches to
// the operand's `ToString`, and the hole path asks whether the OPERAND's type
// may carry a spec at all). The explicit member call `b.ToString("F4")` was
// NOT: the call resolved, was typed, and rendered the value's no-format text.
// The two spellings of one rule disagreed.
//
// THE FIX. A compile-time gate in sema, on the member-call path, that asks
// the same question the hole path asks — of the RECEIVER'S type — and rejects
// the call when the receiver's type has no vocabulary.
//
// WHAT THIS IS **NOT**. The gate does not decide whether a particular
// SPECIFIER is recognized: an unrecognized specifier on a formattable
// receiver is still a RUNTIME `FormatException` (stdlib §2.18, spec §18.5),
// and the row below pins that. Nor does it touch the RUNTIME spelling of the
// defect — a format string whose content is unknown at compile time
// (`b.ToString(fmt)`), which is issue #7336's, and which must still compile:
// the rows below pin that too.
//
// THE ORACLE. A rejection is pinned by a diagnostic at a REAL location
// (`failedAtCompileTimeWithRealLocation`) plus the diagnostic's TEXT — never
// by an exit code, and never by the absence of a diagnostic on a program that
// compiles (a successful compile's stderr is not in `r.diagnostics`, so such
// an assertion would be vacuous). The compiling rows pin the program's exact
// stdout and its exit code instead.
//
// Every row is on `runAot` — a compile-time rejection needs no sanitizer, and
// the `checked_calls` ratchet (`scripts/check_toolchain_skip_surface.py`) must
// not grow.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

// A program whose `Main` body is `body`.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The stdout of a program that compiled and ran; empty when it did not run.
std::string runStdout(const std::string& p) {
    JitResult r = runAot(p);
    return r.ran ? r.stdoutText : std::string();
}

// A rejected program: the diagnostic must carry a REAL location (so the
// rejection is a genuine front-end diagnostic, not a crash / signal /
// timeout) and must say what §2.18 says.
bool rejectedWith(const std::string& p, const char* needle) {
    JitResult r = runAot(p);
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(r, needle);
}

}  // namespace

// ===========================================================================
// THE REJECT ROWS — a format spec on a receiver whose type has no vocabulary.
// ===========================================================================

// stdlib §2.18: the `bool` row of the specifier table — the vocabulary is
// empty, so a format spec on a `bool` is a compile error. The issue's exact
// repro: this compiled and printed "True".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_bool_receiver_format_spec_is_a_compile_error_2_18) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var b := true;\n"
             "    Console.WriteLine(b.ToString(\"F4\"));\n"),
        "`bool` is neither"));
}

// stdlib §2.18: the `char` row — the same empty vocabulary. This compiled and
// printed "a".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_char_receiver_format_spec_is_a_compile_error_2_18) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var c := 'a';\n"
             "    Console.WriteLine(c.ToString(\"F4\"));\n"),
        "`char` is neither"));
}

// stdlib §2.18's neighbouring sentence — "a format spec on any other
// non-conforming operand is a compile error" — and the `Object` receiver: it
// is in no vocabulary row and does not implement `IFormattable`, so it is
// rejected exactly as the interpolation hole `$"{o:F4}"` already was.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_object_receiver_format_spec_is_a_compile_error_2_18) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var o: Object := true;\n"
             "    Console.WriteLine(o.ToString(\"F4\"));\n"),
        "`Object` is neither"));
}

// spec §18.5: a format spec on a NULLABLE-typed operand is checked at
// compile time against the NON-NULLABLE form of the operand's type — so the
// `bool?` receiver is checked as `bool`, and the `?.` spelling is rejected
// too.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_nullable_bool_receiver_format_spec_is_a_compile_error_18_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var b: bool? := true;\n"
             "    Console.WriteLine(b?.ToString(\"F4\"));\n"),
        "`bool` is neither"));
}

// The same rule for the `char?` receiver.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_nullable_char_receiver_format_spec_is_a_compile_error_18_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var c: char? := 'a';\n"
             "    Console.WriteLine(c?.ToString(\"F4\"));\n"),
        "`char` is neither"));
}

// THE HOLE PATH IS UNCHANGED — spec §18.5's dispatch rule makes a hole's
// spec a call to the operand's `ToString`, so the hole and the explicit
// member call are one obligation and must agree. (This row also fails
// WITHOUT the fix, so it is not a fail-without row: it pins that the fix
// did not disturb the hole path.)
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_bool_interpolation_hole_format_spec_is_a_compile_error_18_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    Console.WriteLine($\"{true:F4}\");\n"),
        "a format spec requires an intrinsically-formattable operand"));
}

// THE STRING RECEIVER IS STILL REJECTED — but by stdlib §9.6's ARITY rule
// ("§9.6 declares `ToString(): string`", at arity 0), not by §2.18's
// vocabulary rule. This row pins that the existing error is not disturbed:
// the fix must not turn one mistake into two diagnostics, and the spec does
// not say which of the two rules owns the string receiver.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_string_receiver_format_spec_is_still_the_9_6_arity_error) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("    var s := \"hi\";\n"
             "    Console.WriteLine(s.ToString(\"F4\"));\n"),
        "stdlib §9.6 declares `ToString(): string`"));
}

// ===========================================================================
// THE CONTROL ROWS — the fix must not over-reject.
// ===========================================================================

// stdlib §9.4: `bool` declares `public pure method ToString(): string;` —
// the NO-argument form, with no specifier. It compiles and renders "True".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_bool_receiver_without_a_format_spec_still_compiles_9_4) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var b := true;\n"
             "    Console.WriteLine(b.ToString());\n")),
        "True\n");
}

// stdlib §9.5: the same for `char` — "a".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_char_receiver_without_a_format_spec_still_compiles_9_5) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var c := 'a';\n"
             "    Console.WriteLine(c.ToString());\n")),
        "a\n");
}

// stdlib §9.6: the same for `string` — "hi".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_string_receiver_without_a_format_spec_still_compiles_9_6) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var s := \"hi\";\n"
             "    Console.WriteLine(s.ToString());\n")),
        "hi\n");
}

// stdlib §2.18's integer row: "`D`/`Dn` decimal (n = min digits, zero-padded)"
// — 42 with "D5" is "00042". A legitimate specifier must NOT be rejected.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_int_receiver_with_a_D_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var i := 42;\n"
             "    Console.WriteLine(i.ToString(\"D5\"));\n")),
        "00042\n");
}

// stdlib §2.18's integer row: "`X`/`x` hex (upper/lower, `Xn` pads)" — 42 with
// "X" is "2A".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_int_receiver_with_an_X_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var i := 42;\n"
             "    Console.WriteLine(i.ToString(\"X\"));\n")),
        "2A\n");
}

// stdlib §2.18's integer row: "`B` binary". The row pins that a LEGITIMATE
// specifier is not rejected by the new gate — and NOTHING else.
//
// ⚠ THE RENDERING IS NOT PINNED, because the member-call path does not
// honour "B" TODAY: `42.ToString("B")` renders "42", where §2.18's answer
// is "101010" (the INTERPOLATION hole `$"{42:B}"` does render "101010"). That
// is a SEPARATE, receiver-independent defect in the member-call rendering —
// it fires identically without this fix, it lives in the code-generation
// layer, and it is REPORTED, not fixed, here. Pinning today's text would
// enshrine a wrong rendering; pinning the spec's text would fail the row.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_int_receiver_with_a_B_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var i := 42;\n"
             "    var s: string := i.ToString(\"B\");\n"
             "    Console.WriteLine(\"compiled\");\n")),
        "compiled\n");
}

// stdlib §2.18's integer row, on the UNSIGNED receiver: 255 with "D" is
// "255".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_uint_receiver_with_a_D_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var u: uint := 255;\n"
             "    Console.WriteLine(u.ToString(\"D\"));\n")),
        "255\n");
}

// stdlib §2.18's floating row: "`F`/`Fn` fixed-point (n decimals, default 2)"
// — 5.0 with "F4" is "5.0000".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_double_receiver_with_an_F_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var d := 5.0;\n"
             "    Console.WriteLine(d.ToString(\"F4\"));\n")),
        "5.0000\n");
}

// stdlib §2.18's floating row: "`E`/`En` scientific" — 5.0 with "E" is
// "5.000000E+00" (the digit count is the house's shared rendering's, six;
// §2.18 fixes the default only for `F`).
//
// THE TEXT MOVED: the member spelling used to render "5.00e+00" — two
// decimals, lower-case exponent — while the interpolation spelling rendered
// "5.000000E+00". The member spelling is now routed through the SHARED
// formatting routine (#7311), so the two spellings of one specifier AGREE,
// and this row pins the shared answer.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_double_receiver_with_an_E_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var d := 5.0;\n"
             "    Console.WriteLine(d.ToString(\"E\"));\n")),
        "5.000000E+00\n");
}

// stdlib §2.18's `decimal` row: "`F`/`Fn`, `E`/`En`, `G`, `P`/`Pn`" — 1.5m
// with "F2" is "1.50".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_decimal_receiver_with_an_F_spec_still_compiles_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var d := 1.5m;\n"
             "    Console.WriteLine(d.ToString(\"F2\"));\n")),
        "1.50\n");
}

// THE RUNTIME SPELLING IS #7336's, AND IT STILL COMPILES. A format string
// whose content is unknown at compile time cannot be a compile-time
// rejection: stdlib §2.18 makes an unrecognized specifier a RUNTIME
// `FormatException`, and that is where this spelling belongs. The program
// COMPILES (the call is well-typed) and the `FormatException` is raised at
// RUN time.
//
// THE VERDICT MOVED (#7336): the program used to compile and print
// "compiled" — the no-format rendering ("True") — which is neither the
// compile error the LITERAL spelling gives nor the fault the rule owns.
// stdlib §2.18's last row gives `bool` an EMPTY vocabulary ("| `bool`,
// `char`, `string` | *(none)* | a format spec on these is a compile error |
// "), so no format argument can be recognized on it, and the rule for an
// unrecognized specifier is the fault: "An unrecognized specifier raises
// `FormatException` (strict-fault, as everywhere)".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_bool_receiver_with_a_runtime_format_raises_format_exception_7336) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var b := true;\n"
             "    var fmt: string := \"F4\";\n"
             "    var result := 0;\n"
             "    try\n"
             "      var s: string := b.ToString(fmt);\n"
             "      result := 99;\n"
             "    catch FormatException => result := 11;\n"
             "    end;\n"
             "    Console.WriteLine(result);\n")),
        "11\n");
}

// The same for the `char` receiver — stdlib §2.18's last row gives `char`
// the same EMPTY vocabulary.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_char_receiver_with_a_runtime_format_raises_format_exception_7336) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var c := 'a';\n"
             "    var fmt: string := \"F4\";\n"
             "    var result := 0;\n"
             "    try\n"
             "      var s: string := c.ToString(fmt);\n"
             "      result := 99;\n"
             "    catch FormatException => result := 11;\n"
             "    end;\n"
             "    Console.WriteLine(result);\n")),
        "11\n");
}

// spec §18.5: a hole WITHOUT a format spec formats via the value's ordinary
// `ToString()` — `$"{true}"` prints "True", and this row pins that the fix
// did not touch the no-spec hole.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           a_bool_interpolation_hole_without_a_spec_still_compiles_18_5) {
    BEZEL_EXPECT_EQ(runStdout(prog("    Console.WriteLine($\"{true}\");\n")),
                    "True\n");
}

// spec §18.5 + stdlib §2.18: the hole WITH a spec on a receiver that DOES
// have a vocabulary — `$"{42:D5}"` prints "00042".
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_int_interpolation_hole_with_a_D_spec_still_compiles_18_5) {
    BEZEL_EXPECT_EQ(runStdout(prog("    Console.WriteLine($\"{42:D5}\");\n")),
                    "00042\n");
}

// THE SPECIFIER-RECOGNITION RULE IS STILL THE RUNTIME RULE. "Q" is not in
// stdlib §2.18's integer vocabulary, so the call COMPILES; the spec puts the
// unrecognized specifier's `FormatException` at RUN time (stdlib §2.18, spec
// §18.5). The fix is the type-formattability check and must not become a
// specifier check — a compile-time rejection here would take the fault away
// from the rule that owns it.
//
// ⚠ THE RUNTIME VERDICT IS NOT PINNED: today the member-call path renders the
// value's own text for an unrecognized literal spec instead of raising
// (`42.ToString("Q")` prints "42"), which is a SEPARATE defect in the
// rendering layer — REPORTED, not fixed, here. The row pins only that the
// program COMPILES AND RUNS, and it stays true whichever way the runtime
// verdict goes.
BEZEL_TEST(format_spec_receiver_vocabulary_7333,
           an_unrecognized_spec_on_a_formattable_receiver_is_not_a_compile_error_2_18) {
    BEZEL_EXPECT_EQ(runStdout(
        prog("    var result := 0;\n"
             "    try\n"
             "      var s: string := (42).ToString(\"Q\");\n"
             "      result := 99;\n"
             "    catch FormatException => result := 11;\n"
             "    end;\n"
             "    Console.WriteLine(\"compiled\");\n")),
        "compiled\n");
}
