// Regression tests for #7300 (stdlib §9.3, v0.344): the `decimal` receiver's
// `Parse` / `TryParse` arms are a SECOND COPY of the ones in
// LlvmBackend_Calls2.cpp, in LlvmBackend_Calls.cpp, and that copy had NO
// first-argument guard.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.3 (v0.344) declares on Decimal:
//
//   public static pure method Parse(s: string): decimal;
//   public static method TryParse(s: string, out result: decimal): bool;
//   public static pure method Parse(s: ReadOnlyStringSpan): decimal;
//   public static method TryParse(s: ReadOnlyStringSpan, out result: decimal): bool;
//
// The first parameter is a `string` (or the span overload's
// `ReadOnlyStringSpan`). spec §22.6 (v0.344): "If no candidate is applicable
// at all — no candidate is reachable: a conversion §5.1 does not admit as
// implicit … the call is a compile error with no candidates cited."
//
// ─── The defect ────────────────────────────────────────────────────────────
// MEASURED on the base arm (`r35-parse-arms` tip 54bcb306, `build/bezelc`):
//
//   decimal.TryParse(1.5, out m)   COMPILER ABORT
//       Load operand must be a pointer.
//         %str.data = load ptr, double 1.500000e+00, align 8
//       Call parameter type does not match function signature!
//         %dec.tryp.ok = call i32 @bz_decimal_try_parse(double 1.500000e+00, ptr %1)
//       bezelc: codegen error: IR verification failed (malformed IR emitted)
//   decimal.Parse(1.5)             COMPILER ABORT
//       Call parameter type does not match function signature!
//         call void @bz_decimal_parse(double 1.500000e+00, ptr %2, ...)
//   Decimal.Parse(1.5)             COMPILER ABORT (the same message)
//   decimal.TryParse(o, out m)     COMPILED, printed "False" / "0"  <- the
//                                 object's OWN BYTES were read as text
//   decimal.Parse(o)               COMPILED, printed "Unhandled exception:
//                                 System.FormatException"  <- the object's
//                                 own bytes were read and parsed as TEXT
//                                 (foreign memory)
//
// WHY A GUARD IN LlvmBackend_Calls2.cpp IS NOT ENOUGH: the KEYWORD spelling
// `decimal.TryParse` enters the Calls2 arm FIRST (sema's
// `primitiveFromName` maps "decimal" AND "Decimal" to the same primitive),
// that arm DECLINES the operand once it is guarded, and then this file's
// copy runs next and aborts. So the copy needs the SAME guard, or the
// symptom merely moves.
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (§9.3 declares `s: string`), an illegal program has no
// specified behaviour, and a refusal would be a NEW REJECTION of programs
// that compile today. The call is dropped the way the compiler already drops
// a call it cannot lower — with the SAME shared guard as every other
// `Parse` / `TryParse` arm.
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The compiler's outcome (the program must compile, link and run, and the
// compiler must never reach LLVM's IR verifier) AND the program's output.
//
// The `out` slot is the discriminator. A call that RAN and failed writes the
// failure default into the slot (default(T) — stdlib §1's Try convention,
// shape (c): "bool + non-nullable value-typed out"); a DROPPED call never
// writes it, so it keeps its pre-set value. The rows pre-set the slot to 7,
// so the fixed arm prints 7 where a call that ran and failed printed 0.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
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
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "Load operand must be a pointer"));
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

// A `TryParse(s, out result)` program. The `out` slot is PRE-SET to 7, so a
// DROPPED call (the slot keeps 7) is distinguishable from a call that RAN
// and failed (the slot receives the default 0).
std::string tryParseProg(const std::string& receiver, const std::string& first,
                          const std::string& decl = "", bool obj = false) {
    return prog("    var m: decimal := 7;\n" + decl +
                "    var ok: bool := " + receiver + ".TryParse(" + first +
                ", out m);\n"
                "    Console.WriteLine(ok);\n"
                "    Console.WriteLine(m);\n",
                obj);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a non-`string` first argument must not abort the compiler.
// ---------------------------------------------------------------------------

// `decimal.TryParse(1.5, out m)`. On the base arm the compiler ABORTED with
// "Call parameter type does not match function signature" / "IR verification
// failed (malformed IR emitted)". The `out` slot keeps its PRE-SET 7 — a
// DROPPED call never writes it — where a call that ran and failed wrote the
// failure default 0.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_tryparse_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("decimal", "1.5"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}

// `decimal.Parse(1.5)`. On the base arm the compiler ABORTED with "Call
// parameter type does not match function signature" (`call void
// @bz_decimal_parse(double 1.500000e+00, ...)`).
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_parse_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(prog(
        "    var m := decimal.Parse(1.5);\n"
        "    Console.WriteLine(m);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// The CLASS-NAME spelling `Decimal.Parse(1.5)` — the same copy of the arm.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_class_name_parse_first_argument_does_not_abort) {
    JitResult r = runAot(prog(
        "    var m := Decimal.Parse(1.5);\n"
        "    Console.WriteLine(m);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand.
// ---------------------------------------------------------------------------

// A class instance lowers to a plain pointer, exactly like a `string`, so
// the SHAPE test alone cannot tell them apart. On the base arm the object's
// OWN BYTES were read as text and parsed — foreign memory — and the program
// reported a FormatException for it.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_class_instance_is_not_parsed_as_text) {
    JitResult r = runAot(prog(
        "    var m := decimal.Parse(o);\n"
        "    Console.WriteLine(m);\n",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// The same for the `TryParse` form. On the base arm it printed "False" /
// "0" — the `out` slot received the failure default, because the object's
// bytes had been read and failed to parse.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_tryparse_class_instance_is_not_parsed_as_text) {
    JitResult r = runAot(tryParseProg("decimal", "o", "", /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL: a `string` first argument still parses.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_string_first_argument_still_parses) {
    JitResult r = runAot(prog(
        "    var m := decimal.Parse(\"1.5\");\n"
        "    Console.WriteLine(m);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}

// The `TryParse` positive control — the call still RUNS and writes the
// parsed value into the `out` slot.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_tryparse_string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("decimal", "\"1.5\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}

// The SPAN overload (stdlib §9.3's `ReadOnlyStringSpan` form) still parses —
// the guard admits the {ptr,i32,i32} record by its shape.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_tryparse_span_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg(
        "decimal", "sp",
        "    var sp := new ReadOnlyStringSpan(\"1.5\", 0, 3);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}

// The `Parse` span overload still parses too.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_parse_span_first_argument_still_parses) {
    JitResult r = runAot(prog(
        "    var sp := new ReadOnlyStringSpan(\"1.5\", 0, 3);\n"
        "    var m := decimal.Parse(sp);\n"
        "    Console.WriteLine(m);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}

// The CLASS-NAME spelling's positive control.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_class_name_string_first_argument_still_parses) {
    JitResult r = runAot(prog(
        "    var m := Decimal.Parse(\"1.5\");\n"
        "    Console.WriteLine(m);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}

// The CLASS-NAME spelling's `TryParse` positive control.
BEZEL_TEST(decimal_parse_first_arg_7300,
           decimal_class_name_tryparse_string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("Decimal", "\"1.5\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}
