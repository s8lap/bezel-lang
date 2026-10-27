// Regression tests for #7299 (stdlib §9.4 / §9.5, v0.344): the `bool` and
// `char` receivers of `Parse` / `TryParse` / `ParseOrNull` had NO
// first-argument guard, so a non-`string` first argument ABORTED the
// compiler.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.4 (v0.344) declares on Boolean:
//
//   public static pure method Parse(s: string): bool;
//   public static method TryParse(s: string, out result: bool): bool;
//   public static pure method ParseOrNull(s: string): bool?;
//
// stdlib §9.5 (v0.344) declares on Char:
//
//   public static pure method Parse(s: string): char;
//   public static method TryParse(s: string, out result: char): bool;
//   public static pure method ParseOrNull(s: string): char?;
//
// The first parameter is a `string`, and §9.4 / §9.5 declare NO
// `ReadOnlyStringSpan` overload (unlike §9.1 / §9.2 / §9.3, which do —
// stdlib §9.1: "the span forms are per-type members"). spec §5.1's
// implicit-conversion table has no span-to-string row either, so a span
// operand is not a parseable operand at these receivers.
//
// spec §22.6 (v0.344): "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit … the call is a
// compile error with no candidates cited."
//
// ─── The defect ────────────────────────────────────────────────────────────
// MEASURED on the base arm (`r35-parse-arms` tip 54bcb306, `build/bezelc`):
//
//   bool.Parse(16)            COMPILER ABORT
//       Load operand must be a pointer.
//         %str.data = load ptr, i32 16, align 8
//       bezelc: codegen error: IR verification failed (malformed IR emitted)
//   bool.TryParse(16, out b)  COMPILER ABORT (the same message)
//   char.Parse(16)            COMPILER ABORT
//       %str.len = load i64, i32 getelementptr inbounds nuw (%bz.string, i32 16, ...)
//       %str.data = load ptr, i32 16, align 8
//       %str.width = load i8, i32 getelementptr inbounds nuw (%bz.string, i32 16, ...)
//       bezelc: codegen error: IR verification failed (malformed IR emitted)
//   char.TryParse(16, out c)  COMPILER ABORT (the same three messages)
//   bool.ParseOrNull(16)      COMPILER ABORT (the same message)
//   char.ParseOrNull(16)      COMPILER ABORT (the same three messages)
//   Boolean.TryParse(16, out b) COMPILER ABORT — the CLASS-NAME spelling runs
//       the Calls2 arm first, that arm declines the operand, and then this
//       file's SECOND COPY of the arm in LlvmBackend_Calls.cpp runs and
//       aborts (#7300's shape: the symptom would merely move)
//   bool.Parse(span)          COMPILER ABORT ("GEP base pointer is not a
//       vector or a vector of pointers" / "Load operand must be a pointer")
//   char.Parse(span)          COMPILER ABORT (the same shape)
//
// And the WORSE case — a CLASS INSTANCE, which lowers to a plain pointer,
// exactly like a `string` — was read as TEXT out of the object's OWN BYTES
// (foreign memory), silently:
//
//   bool.Parse(o)     "Unhandled exception: System.FormatException"
//   char.Parse(o)     "Unhandled exception: System.FormatException"
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (§9.4 / §9.5 declare `s: string`), an illegal program has
// no specified behaviour, and a refusal would be a NEW REJECTION of programs
// that compile today. The call is dropped the way the compiler already drops
// a call it cannot lower (#7294's remedy, the SAME shared guard).
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The compiler's outcome (the program must compile, link and run, and the
// compiler must never reach LLVM's IR verifier) AND the program's output.
//
// The `out` slot is the discriminator. A call that RAN and failed writes the
// failure default into the slot (default(T) — stdlib §1's Try convention,
// shape (c): "bool + non-nullable value-typed out"; §9.5 states char's
// explicitly: "the out slot gets '\0' — default(char)"). A DROPPED call
// never writes it, so it keeps its pre-set value. So the rows pre-set the
// slot to `true` (bool) / 'z' (char) and the fixed arm prints the PRE-SET
// value, where a call that ran and failed would print the default.

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

// A `TryParse(s, out result)` program. The `out` slot is PRE-SET, so a
// DROPPED call (the slot keeps the pre-set value) is distinguishable from a
// call that RAN and failed (the slot receives the default).
std::string tryParseProg(const std::string& receiver,
                          const std::string& slotTy,
                          const std::string& slotInit,
                          const std::string& first,
                          const std::string& printed) {
    return prog("    var v: " + slotTy + " := " + slotInit + ";\n"
                "    var ok: bool := " + receiver + ".TryParse(" + first +
                ", out v);\n"
                "    Console.WriteLine(ok);\n"
                "    Console.WriteLine(" + printed + ");\n");
}

// A `ParseOrNull(s)` program — the nullable result, printed as "some" /
// "null".
std::string parseOrNullProg(const std::string& receiver,
                             const std::string& first) {
    return prog("    var v := " + receiver + ".ParseOrNull(" + first + ");\n"
                "    if v = null then Console.WriteLine(\"null\"); "
                "else Console.WriteLine(\"some\"); end;\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a non-`string` first argument must not abort the compiler.
// ---------------------------------------------------------------------------

// `bool.Parse(16)`. On the base arm the compiler ABORTED with "Load operand
// must be a pointer" / "IR verification failed (malformed IR emitted)".
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(prog(
        "    var v := bool.Parse(16);\n"
        "    Console.WriteLine(v);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// `bool.TryParse(16, out b)`. On the base arm the compiler ABORTED. The
// `out` slot keeps its PRE-SET `true` — a DROPPED call never writes it —
// where a call that ran and failed would have written default(bool) = false.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_tryparse_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("bool", "bool", "true", "16", "v"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

// `char.Parse(16)`. On the base arm the compiler ABORTED with three load
// errors (str.len / str.data / str.width) and then "IR verification
// failed".
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(prog(
        "    var v := char.Parse(16);\n"
        "    Console.WriteLine(int(v));\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// `char.TryParse(16, out c)`. On the base arm the compiler ABORTED. The
// `out` slot keeps its PRE-SET 'z' (122) — stdlib §9.5: a call that RAN and
// failed writes '\0' (default(char)) into it.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_tryparse_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("char", "char", "'z'", "16", "int(v)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n122\n"));
}

// `bool.ParseOrNull(16)` — the same class of defect in the *OrNull tier.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_parse_or_null_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(parseOrNullProg("bool", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null\n"));
}

// `char.ParseOrNull(16)` — the same class of defect in the *OrNull tier.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_parse_or_null_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(parseOrNullProg("char", "16"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null\n"));
}

// The CLASS-NAME spelling `Boolean.TryParse(16, out b)`. This is the
// SECOND COPY of the arm, in LlvmBackend_Calls.cpp: the Calls2 arm runs
// first, declines the operand, and this copy ran next and ABORTED. The
// guard has to be here too, or the symptom merely moves (#7300's shape).
BEZEL_TEST(parse_bool_char_first_arg_7299,
           boolean_tryparse_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("Boolean", "bool", "true", "16", "v"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand.
// ---------------------------------------------------------------------------

// A class instance lowers to a plain pointer, exactly like a `string`, so
// the SHAPE test alone cannot tell them apart. On the base arm the object's
// OWN BYTES were read as text and parsed — foreign memory — and the program
// reported a FormatException for it.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_class_instance_is_not_parsed_as_text) {
    JitResult r = runAot(prog(
        "    var v := bool.Parse(o);\n"
        "    Console.WriteLine(v);\n",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// The same for the `char` receiver.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_class_instance_is_not_parsed_as_text) {
    JitResult r = runAot(prog(
        "    var v := char.Parse(o);\n"
        "    Console.WriteLine(int(v));\n",
        /*obj=*/true));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// THE PIN: a span is not a parseable operand at these receivers.
// ---------------------------------------------------------------------------

// stdlib §9.4 declares NO `ReadOnlyStringSpan` overload of `bool.Parse`, and
// spec §5.1 has no span-to-string implicit conversion, so `bool.Parse(span)`
// is an illegal program. On the base arm it ABORTED the compiler ("GEP base
// pointer is not a vector or a vector of pointers" / "Load operand must be
// a pointer"); the guard DROPS it.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_span_operand_is_not_a_parseable_operand) {
    JitResult r = runAot(prog(
        "    var sp := new ReadOnlyStringSpan(\"true\", 0, 4);\n"
        "    var v := bool.Parse(sp);\n"
        "    Console.WriteLine(v);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// The same for the `char` receiver.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_span_operand_is_not_a_parseable_operand) {
    JitResult r = runAot(prog(
        "    var sp := new ReadOnlyStringSpan(\"a\", 0, 1);\n"
        "    var v := char.Parse(sp);\n"
        "    Console.WriteLine(int(v));\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL: a `string` first argument still parses.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_string_first_argument_still_parses) {
    JitResult r = runAot(prog(
        "    var v := bool.Parse(\"true\");\n"
        "    Console.WriteLine(v);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// The `char` receiver's positive control.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_string_first_argument_still_parses) {
    JitResult r = runAot(prog(
        "    var v := char.Parse(\"a\");\n"
        "    Console.WriteLine(int(v));\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("97\n"));
}

// The `bool` receiver's `TryParse` positive control — the call still RUNS
// and writes the parsed value into the `out` slot.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_tryparse_string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("bool", "bool", "true", "\"true\"", "v"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n"));
}

// The `char` receiver's `TryParse` positive control.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_tryparse_string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("char", "char", "'z'", "\"a\"", "int(v)"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n97\n"));
}

// The `bool` receiver's `ParseOrNull` positive control — the nullable result
// is PRESENT.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           bool_parse_or_null_string_first_argument_still_parses) {
    JitResult r = runAot(parseOrNullProg("bool", "\"true\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("some\n"));
}

// The `char` receiver's `ParseOrNull` positive control.
BEZEL_TEST(parse_bool_char_first_arg_7299,
           char_parse_or_null_string_first_argument_still_parses) {
    JitResult r = runAot(parseOrNullProg("char", "\"a\""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("some\n"));
}
