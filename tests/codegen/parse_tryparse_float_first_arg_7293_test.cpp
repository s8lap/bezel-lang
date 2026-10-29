// Regression tests for #7293 (stdlib §9.2, v0.344): the floating-point
// `TryParse(s, out result)` lowering arms loaded their FIRST argument as a
// `string` without checking that it WAS one.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.2 (v0.344) declares the floating-point parse forms:
//
//   public static pure method Parse(s: string): double;
//   public static method TryParse(s: string, out result: double): bool;
//
// (and the `float` shapes, analogous). The first parameter is a `string`,
// and the form also has a `ReadOnlyStringSpan` overload.
//
// spec §22.6 (v0.344): "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit … the call is a
// compile error with no candidates cited."
//
// ─── The defect ────────────────────────────────────────────────────────────
// The arms lowered whatever they were handed into a string operand, so a
// `double` first argument produced malformed IR and the COMPILER ABORTED:
//
//   double.TryParse(1.5, out d)
//     Load operand must be a pointer.
//       %str.data = load ptr, double 1.500000e+00, align 8
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// and the same for `float`. A CLASS INSTANCE — which lowers to a plain
// pointer, exactly like a `string` — was read as TEXT out of the object's
// own bytes, silently.
//
// MEASURED on the base arm (the staging tip this lane started from):
//
//   double.TryParse(1.5, out d)   COMPILER ABORT (IR verification failed)
//   float.TryParse(1.5f, out f)   COMPILER ABORT (IR verification failed)
//   double.TryParse(16, out d)    COMPILER ABORT (IR verification failed)
//   double.TryParse(o, out d)     COMPILED, printed "False" / "0"  <- the
//                                 object's own bytes were read as text
//
// The integer tower's `TryParse` arm had already been given a first-argument
// guard (the #7285 fix-forward); this is the same hole in the
// floating-point siblings.
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (stdlib §9.2 declares `s: string`), and a refusal would be
// a NEW REJECTION of a program that compiles today.
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The compiler's outcome (the program must compile, link and run, and the
// compiler must never reach LLVM's IR verifier) AND the program's output.
// The `out` slot starts at 7, so the two behaviours are distinguishable: a
// call that RAN and failed writes the failure default (0: default(T), stdlib
// §1 shape (c), spec §17.11) into it, and a
// DROPPED call never writes it, so it keeps 7.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A `TryParse(s, out result)` program for the floating-point receivers.
// `first` is the operand stdlib §9.2 declares as `s: string`; the `out` slot
// starts at 7 so that a DROPPED call (the slot keeps 7) is distinguishable
// from a call that ran and failed (the slot receives default(T) = 0, stdlib
// §1 shape (c)).
std::string tryParseProg(const std::string& receiver, const std::string& slotTy,
                          const std::string& slotInit, const std::string& first,
                          const std::string& decl) {
    return "module M;\n"
           "class C2\n"
           "  public F: int := 3;\n"
           "end C2;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var o := new C2();\n" + decl +
           "    var v: " + slotTy + " := " + slotInit + ";\n"
           "    var ok: bool := " + receiver + ".TryParse(" + first +
           ", out v);\n"
           "    Console.WriteLine(ok);\n"
           "    Console.WriteLine(v);\n"
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


// THE REFUSAL (campaign 2, `builtin-call-admission`): §22.6 (v0.344) — "If
// no candidate is applicable at all ... the call is a compile error with no
// candidates cited". The declared surface of the standard library (stdlib
// §9.1-§9.5, §2.5, §2.6) declares the FIRST parameter of this member as a
// `string`, and §5.1 admits no conversion to `string` from the operand these
// rows pass — so the call reaches no candidate and the compiler REFUSES it,
// at a real location, with that one diagnostic.
//
// The oracle is the diagnostic text plus a real location, never the absence
// of a diagnostic.
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a non-string first argument must not abort the compiler.
// ---------------------------------------------------------------------------

// A `double` first argument. On the base arm the compiler ABORTED with
// "IR verification failed (malformed IR emitted)".
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           double_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("double", "double", "7.0", "1.5", ""));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

// The same for `float` — the arm is shared, but the literal must be a
// `float` literal (a `double` literal is not implicitly convertible to a
// `float` — §5.1).
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           float_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("float", "float", "7.0f", "1.5f", ""));
    expectRefused(r, "no matching overload of `float.TryParse`");
}

// An `int` first argument — the same shape, a different type.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           int_first_argument_does_not_abort_the_compiler) {
    JitResult r = runAot(tryParseProg("double", "double", "7.0", "16", ""));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand.
// ---------------------------------------------------------------------------

// A class instance lowers to a plain pointer, exactly like a `string`, so
// the shape test alone cannot tell them apart. On the base arm the object's
// OWN BYTES were read as text and the `out` slot received the failure
// default 0; with the guard the call is dropped and the slot keeps 7.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           class_instance_first_argument_is_not_parsed_as_text) {
    JitResult r = runAot(tryParseProg("double", "double", "7.0", "o", ""));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

// The same for the `float` receiver.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           float_class_instance_first_argument_is_not_parsed_as_text) {
    JitResult r = runAot(tryParseProg("float", "float", "7.0f", "o", ""));
    expectRefused(r, "no matching overload of `float.TryParse`");
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL: a `string` first argument still parses.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("double", "double", "7.0", "\"1.5\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}

// The `float` receiver's positive control.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           float_string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("float", "float", "7.0f", "\"1.5\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}

// The span overload (stdlib §9.2's `ReadOnlyStringSpan` form) still parses —
// the guard admits the {ptr,i32,i32} record by its shape.
BEZEL_TEST(parse_tryparse_float_first_arg_7293,
           span_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg(
        "double", "double", "7.0", "sp",
        "    var sp := new ReadOnlyStringSpan(\"1.5\", 0, 3);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n1.5\n"));
}
