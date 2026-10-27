// Regression tests for #7313 (stdlib §2.5 / §2.6, v0.344): the `Guid`,
// `DateTime`, `DateOnly` and `TimeOnly` `Parse` / `TryParse` arms in
// `src/codegen/LlvmBackend_Calls.cpp` had NO first-argument guard, so a
// non-`string` first argument ABORTED THE COMPILER.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §2.5 (v0.344) declares on DateTime / DateOnly / TimeOnly:
//
//   public static method Parse(s: string): DateTime;
//   public static method TryParse(s: string, out result: DateTime): bool;
//   public static method Parse(s: string): DateOnly;
//   public static method TryParse(s: string, out result: DateOnly): bool;
//   public static method Parse(s: string): TimeOnly;
//   public static method TryParse(s: string, out result: TimeOnly): bool;
//
// stdlib §2.6 (v0.344) declares on Guid:
//
//   public static method Parse(s: string): Guid;
//   public static method TryParse(s: string, out result: Guid): bool;
//
// NONE of these four types declares a `ReadOnlyStringSpan` overload (contrast
// stdlib §9.1 / §9.2 / §9.3, which DO, and §9.4 / §9.5, which do not), so
// the declared first-parameter type is `string` and a non-`string` operand is
// not a parseable operand.
//
// ─── The defect ────────────────────────────────────────────────────────────
// MEASURED on the base arm (`2e9c598e`, `build/bezelc`), all EIGHT arms:
//
//   Guid.Parse(16)             COMPILER ABORT
//       Call parameter type does not match function signature!
//       i32 16
//        ptr  %guid.parse.ok = call i32 @bz_guid_parse(i32 16, ptr %1)
//       bezelc: codegen error: IR verification failed (malformed IR emitted)
//   Guid.TryParse(16, out r)   COMPILER ABORT (same, @bz_guid_try_parse)
//   DateTime.Parse(16)         COMPILER ABORT (same, @bz_datetime_try_parse)
//   DateTime.TryParse(16, out r) COMPILER ABORT (same)
//   DateOnly.Parse(16)         COMPILER ABORT (same)
//   DateOnly.TryParse(16, out r) COMPILER ABORT (same)
//   TimeOnly.Parse(16)         COMPILER ABORT (same)
//   TimeOnly.TryParse(16, out r) COMPILER ABORT (same)
//
// A CLASS INSTANCE as the first argument was a second, quieter symptom: it
// lowers to a plain pointer, exactly like a `string`, so the SHAPE test alone
// cannot tell the two apart, and the object's OWN BYTES were read as TEXT
// (foreign memory).
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (§2.5 / §2.6 declare `s: string`), an illegal program has
// no specified behaviour, and a refusal would be a NEW REJECTION of programs
// that compile today. The call is dropped the way the compiler already drops
// a call it cannot lower — with the SAME shared guard
// (`sharedParseOperandAdmissible`) as every other `Parse` / `TryParse` arm,
// with `spanOverloadDeclared = false` because none of these four types
// declares a span overload.
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The COMPILER'S OUTCOME (the program must compile, link and run, and the
// compiler must never reach LLVM's IR verifier) AND the program's OUTPUT.
//
// The `out` slot is the discriminator. A DROPPED call never writes it, so it
// keeps its pre-set value; a call that RAN and failed writes the failure
// default into the slot (`default(T)` — stdlib §1's Try convention, shape (c)).
// The rows pre-set the slot to a distinguishable value, so the fixed arm
// prints that value where a call that ran and failed printed the default.
//
// ⚠ THE `default(T)` HALF OF THAT RULE IS NOT UNIVERSAL: `Guid` IS THE
// EXCEPTION. `bz_guid_try_parse` returns 0 WITHOUT writing the `out` slot on
// failure — `guid_parse_str` copies its output only on success, and the
// sibling `bz_guid_parse` memsets its output on failure where the try form
// does not (src/runtime/bezel_runtime_decimal_guid.cpp) — so a `Guid`
// `TryParse` that ran and failed ALSO leaves the slot untouched, rather than
// writing `default(Guid)`. That is the open defect #7320, not a property of
// this file's remedy. It is why there is no `Guid.TryParse` class-instance
// row here (see the comment block on the absent class-instance row below):
// for `Guid` the pre-fix and post-fix outputs would be byte-identical, and
// the row would pin nothing.
//
// SO: the failure default is written on the false path for DateTime,
// DateOnly and TimeOnly (their runtimes assign it), and NOT for Guid. The
// DISCRIMINATOR every row here relies on is the DROPPED-call half, which
// holds for all four receivers without exception.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The COMPILER'S OUTCOME. The program must compile, link and RUN, and the
// compiler must never reach LLVM's IR verifier.
void expectCompiledWithoutIRFailure(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "malformed IR"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

// A program whose body is `body`. `obj` adds the class instance the
// foreign-memory rows need.
std::string prog(const std::string& body, bool obj = false) {
    std::string src = "module M;\n"
                      "class C2\n"
                      "  public F: int := 3;\n"
                      "end C2;\n"
                      "class C\n"
                      "  public static method Main(): int\n";
    if (obj) src += "    var o := new C2();\n";
    src += body;
    src += "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
    return src;
}

// A `<T>.Parse(first)` program. The parsed value is printed through its own
// `ToString()`, so the row pins the VALUE and not merely "it compiled".
std::string parseProg(const std::string& type, const std::string& first,
                       bool obj = false) {
    return prog("    var v: " + type + " := " + type + ".Parse(" + first + ");\n"
                "    Console.WriteLine(v.ToString());\n",
                obj);
}

// A `<T>.TryParse(first, out result)` program. `pre` is the PRE-SET value of
// the `out` slot, so a DROPPED call (the slot keeps its pre-set value) is
// distinguishable from a call that RAN and failed (the slot would receive
// the failure default, `default(T)` — stdlib §1's Try convention, shape (c)).
std::string tryParseProg(const std::string& type, const std::string& first,
                          const std::string& pre, bool obj = false) {
    return prog("    var r: " + type + " := " + pre + ";\n"
                "    var ok: bool := " + type + ".TryParse(" + first + ", out r);\n"
                "    Console.WriteLine(ok);\n"
                "    Console.WriteLine(r.ToString());\n",
                obj);
}

// The legal strings, and the pre-set `out` values, per receiver.
const char kGuidText[] = "11111111-1111-1111-1111-111111111111";
const char kDateTimeText[] = "2020-01-02T03:04:05";
const char kDateOnlyText[] = "2020-01-02";
const char kTimeOnlyText[] = "03:04:05";

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a non-`string` first argument must not abort the compiler.
// ---------------------------------------------------------------------------

// `Guid.Parse(16)`. On the base arm the compiler ABORTED with "Call parameter
// type does not match function signature" / "IR verification failed (malformed
// IR emitted)". The call is DROPPED, so the local keeps the zero-initialised
// default the degraded lowering leaves behind.
BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_parse_non_string_does_not_abort_the_compiler_spec_2_6) {
    JitResult r = runAot(parseProg("Guid", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("00000000-0000-0000-0000-000000000000\n"));
}

// `Guid.TryParse(16, out r)`. On the base arm the compiler ABORTED (same
// message, @bz_guid_try_parse). The DROPPED call never writes the `out` slot,
// so it keeps its PRE-SET value — where a call that ran and failed would have
// written `default(Guid)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_tryparse_non_string_does_not_abort_the_compiler_spec_2_6) {
    JitResult r = runAot(tryParseProg(
        "Guid", "16", std::string("Guid.Parse(\"") + kGuidText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kGuidText + "\n");
}

// `DateTime.Parse(16)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_parse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(parseProg("DateTime", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0001-01-01T00:00:00\n"));
}

// `DateTime.TryParse(16, out r)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_tryparse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateTime", "16", std::string("DateTime.Parse(\"") + kDateTimeText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateTimeText + "\n");
}

// `DateOnly.Parse(16)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_parse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(parseProg("DateOnly", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0001-01-01\n"));
}

// `DateOnly.TryParse(16, out r)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_tryparse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateOnly", "16", std::string("DateOnly.Parse(\"") + kDateOnlyText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateOnlyText + "\n");
}

// `TimeOnly.Parse(16)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_parse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(parseProg("TimeOnly", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00:00:00\n"));
}

// `TimeOnly.TryParse(16, out r)`.
BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_tryparse_non_string_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "TimeOnly", "16", std::string("TimeOnly.Parse(\"") + kTimeOnlyText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kTimeOnlyText + "\n");
}

// ---------------------------------------------------------------------------
// THE PIN: a `double` first argument must not abort the compiler either.
// ---------------------------------------------------------------------------

// The `int` literal above is one non-`string` operand; a `double` is another
// primitive kind, and it lowered the same way — straight into the runtime's
// `ptr` parameter — so it aborted the compiler on the base arm too. The
// `out` slot is PRE-SET, so a DROPPED call (the slot keeps its pre-set value)
// is distinguishable from a call that ran and failed (the slot would receive
// the failure default).
BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_tryparse_double_argument_does_not_abort_the_compiler_spec_2_6) {
    JitResult r = runAot(tryParseProg(
        "Guid", "1.5", std::string("Guid.Parse(\"") + kGuidText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kGuidText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_tryparse_double_argument_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateTime", "1.5", std::string("DateTime.Parse(\"") + kDateTimeText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateTimeText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_tryparse_double_argument_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateOnly", "1.5", std::string("DateOnly.Parse(\"") + kDateOnlyText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateOnlyText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_tryparse_double_argument_does_not_abort_the_compiler_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "TimeOnly", "1.5", std::string("TimeOnly.Parse(\"") + kTimeOnlyText + "\")"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kTimeOnlyText + "\n");
}

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand (foreign memory).
// ---------------------------------------------------------------------------

// A class instance lowers to a plain pointer, exactly like a `string`, so the
// SHAPE test alone cannot tell them apart. Before the guard, the object's OWN
// BYTES were read as text and parsed. The guard rejects it on its STATIC TYPE
// (a class type is not `string`), so the object's bytes are never read.
BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_parse_class_instance_is_not_read_as_text_spec_2_6) {
    JitResult r = runAot(parseProg("Guid", "o", /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("00000000-0000-0000-0000-000000000000\n"));
}

// ⚠ NO `Guid.TryParse` CLASS-INSTANCE ROW, AND THE REASON IS A RUNTIME FACT,
// NOT A TEST ARTEFACT. The three sibling receivers' `TryParse` class-instance
// rows below discriminate because their runtimes WRITE the failure default
// into the `out` slot on the false path (stdlib §1's Try convention, shape
// (c)). `bz_guid_try_parse` does NOT: on failure it returns 0 WITHOUT
// touching `out` (src/runtime/bezel_runtime_decimal_guid.cpp — `bz_guid_parse`
// zeroes its output, `bz_guid_try_parse` does not). So on the base arm the
// `out` slot kept its pre-set value, the bool was false, and the program's
// output was BYTE-IDENTICAL to the fixed arm's — the row was green on BOTH
// arms, an admitting twin that would have pinned nothing. The `Guid.Parse`
// class-instance row above covers the same foreign-memory read (on the base
// arm the read happened and the parse failed, so the program threw and exited
// non-zero), and the `Guid.TryParse` guard itself is pinned by the `int` and
// `double` rows above.


BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_parse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(parseProg("DateTime", "o", /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0001-01-01T00:00:00\n"));
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_tryparse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateTime", "o", std::string("DateTime.Parse(\"") + kDateTimeText + "\")",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateTimeText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_parse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(parseProg("DateOnly", "o", /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0001-01-01\n"));
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_tryparse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateOnly", "o", std::string("DateOnly.Parse(\"") + kDateOnlyText + "\")",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kDateOnlyText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_parse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(parseProg("TimeOnly", "o", /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00:00:00\n"));
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_tryparse_class_instance_is_not_read_as_text_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "TimeOnly", "o", std::string("TimeOnly.Parse(\"") + kTimeOnlyText + "\")",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\n") + kTimeOnlyText + "\n");
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL for each receiver: a `string` first argument still
// parses, and the parsed VALUE is pinned (a `Guid` all-zeros would not
// discriminate — the default `Guid` is the all-zero one).
BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_parse_string_first_argument_still_parses_spec_2_6) {
    JitResult r = runAot(parseProg("Guid", std::string("\"") + kGuidText + "\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kGuidText) + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           guid_tryparse_string_first_argument_still_parses_spec_2_6) {
    JitResult r = runAot(tryParseProg(
        "Guid", std::string("\"") + kGuidText + "\"", "default(Guid)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n") + kGuidText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_parse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(parseProg("DateTime", std::string("\"") + kDateTimeText + "\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kDateTimeText) + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           datetime_tryparse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateTime", std::string("\"") + kDateTimeText + "\"", "default(DateTime)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n") + kDateTimeText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_parse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(parseProg("DateOnly", std::string("\"") + kDateOnlyText + "\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kDateOnlyText) + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           dateonly_tryparse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "DateOnly", std::string("\"") + kDateOnlyText + "\"", "default(DateOnly)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n") + kDateOnlyText + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_parse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(parseProg("TimeOnly", std::string("\"") + kTimeOnlyText + "\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kTimeOnlyText) + "\n");
}

BEZEL_TEST(parse_receiver_first_arg_7313,
           timeonly_tryparse_string_first_argument_still_parses_spec_2_5) {
    JitResult r = runAot(tryParseProg(
        "TimeOnly", std::string("\"") + kTimeOnlyText + "\"", "default(TimeOnly)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n") + kTimeOnlyText + "\n");
}
