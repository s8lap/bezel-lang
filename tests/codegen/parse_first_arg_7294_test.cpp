// Regression tests for #7294 (stdlib §9.1 / §9.2, v0.344): the
// ONE-ARGUMENT `Parse(s)` lowering arm had NO first-argument guard at all.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.1 (v0.344) declares the integer tower's parse form:
//
//   public static pure method Parse(s: string): int;
//
// (and §9.2's `double` / `float` shapes, §9.3's `decimal` one). The first
// parameter is a `string`, and the form also has a `ReadOnlyStringSpan`
// overload.
//
// spec §22.6 (v0.344): "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit … the call is a
// compile error with no candidates cited."
//
// ─── The defect ────────────────────────────────────────────────────────────
// The arm did not even lower its operand into a string operand — it fell
// through to `return arg`, so whatever it was handed came back AS THE PARSE
// RESULT. MEASURED on the base arm (the staging tip this lane started from):
//
//   int.Parse(true)      -> prints 1        (the `bool`, as an `int`)
//   int.Parse('a')       -> prints 97       (the character's own code point)
//   int.Parse(1.5)       -> prints garbage  (2366960 — a `double` bit
//                                            pattern the coercion mangled)
//   long.Parse(true)     -> prints 1
//   double.Parse(true)   -> COMPILER ABORT ("Call parameter type does not
//                            match function signature", `%1 = load i1`)
//
// and, for a CLASS INSTANCE, the WORST case — the arm lowered the object's
// POINTER as a string operand in the `decimal` receiver's path and read the
// OBJECT'S OWN BYTES as a string descriptor, i.e. FOREIGN MEMORY, whose
// content was then parsed as text:
//
//   decimal.Parse(o)     -> "Unhandled exception: System.FormatException"
//                            (the object's bytes were read and failed to
//                             parse)
//
// `int.Parse(o)` printed a HEAP ADDRESS (559997728) — the object's pointer,
// handed back as the parse result.
//
// So the arm printed a wrong answer, or aborted the compiler, or read memory
// the program does not own — and never once said so.
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (stdlib §9.1 declares `s: string`), and a refusal would be
// a NEW REJECTION of a program that compiles today. The call is dropped the
// way the compiler already drops a call it cannot lower, so the program
// receives the degraded value and the internal "failed to lower" note.
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The compiler's outcome (the program must compile, link and run, and the
// compiler must never reach LLVM's IR verifier) AND the program's output.
// A dropped call yields 0 — the degraded value — where the base arm yielded
// a wrong answer, an abort, or a heap address.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// A `Parse(s)` program whose FIRST argument is `first` — the operand stdlib
// §9.1 declares as `s: string`. `slotTy` is the receiver's type.
std::string parseProg(const std::string& receiver, const std::string& slotTy,
                       const std::string& first, const std::string& decl) {
    return "module M;\n"
           "class C2\n"
           "  public F: int := 3;\n"
           "end C2;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var o := new C2();\n" + decl +
           "    var x: " + slotTy + " := " + receiver + ".Parse(" + first +
           ");\n"
           "    Console.WriteLine(x);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

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

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a non-string first argument is not a parseable operand.
// ---------------------------------------------------------------------------

// A `bool` first argument. On the base arm the `bool` came back AS the parse
// result and the program printed 1.
BEZEL_TEST(parse_first_arg_7294, bool_first_argument_is_not_the_parse_result) {
    JitResult r = runAot(parseProg("int", "int", "true", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// A `char` first argument. On the base arm the program printed 97 — the
// character's own code point, handed back as the parse result.
BEZEL_TEST(parse_first_arg_7294, char_first_argument_is_not_the_parse_result) {
    JitResult r = runAot(parseProg("int", "int", "'a'", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// A `double` first argument. On the base arm the program printed garbage
// (2366960 — the `double`'s bit pattern, mangled by the coercion), and in
// the `Console.WriteLine(int.Parse(1.5))` spelling the compiler ABORTED with
// "Call parameter type does not match function signature".
BEZEL_TEST(parse_first_arg_7294, double_first_argument_does_not_abort) {
    JitResult r = runAot(parseProg("int", "int", "1.5", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// The `long` receiver — the arm is shared with the rest of the integer
// tower, so the row pins that the guard is not `int`-specific.
BEZEL_TEST(parse_first_arg_7294, long_bool_first_argument_is_not_the_result) {
    JitResult r = runAot(parseProg("long", "long", "true", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// The `double` receiver. On the base arm the compiler ABORTED here ("Call
// parameter type does not match function signature", `%1 = load i1`).
BEZEL_TEST(parse_first_arg_7294, double_receiver_bool_first_argument) {
    JitResult r = runAot(parseProg("double", "double", "true", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// A CLASS INSTANCE. On the base arm the program printed the object's HEAP
// ADDRESS (559997728) — the pointer, handed back as the parse result.
BEZEL_TEST(parse_first_arg_7294, class_instance_is_not_the_parse_result) {
    JitResult r = runAot(parseProg("int", "int", "o", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL: a `string` first argument still parses.
BEZEL_TEST(parse_first_arg_7294, string_first_argument_still_parses) {
    JitResult r = runAot(parseProg("int", "int", "\"255\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("255\n"));
}

// The span overload (stdlib §9.1's `ReadOnlyStringSpan` form) still parses —
// the guard admits the {ptr,i32,i32} record by its shape.
BEZEL_TEST(parse_first_arg_7294, span_first_argument_still_parses) {
    JitResult r = runAot(parseProg(
        "int", "int", "sp",
        "    var sp := new ReadOnlyStringSpan(\"255\", 0, 3);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("255\n"));
}

// The `long` receiver's positive control.
BEZEL_TEST(parse_first_arg_7294, long_string_first_argument_still_parses) {
    JitResult r = runAot(parseProg("long", "long", "\"255\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("255\n"));
}

// The `double` receiver's positive control.
BEZEL_TEST(parse_first_arg_7294, double_string_first_argument_still_parses) {
    JitResult r = runAot(parseProg("double", "double", "\"1.5\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}
