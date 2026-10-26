// Regression tests for #7290 (stdlib §9.1, v0.344): the two-argument
// `TryParse(s, out result)` lowering arm loaded its FIRST argument as a
// `string` without checking that it WAS one.
//
// ─── The normative sentences ───────────────────────────────────────────────
// stdlib §9.1 (v0.344) declares the integer tower's two parse forms:
//
//   public static pure method Parse(s: string): int;
//   public static method TryParse(s: string, out result: int): bool;
//
// The first parameter is a `string`, and the non-radix form also has a
// `ReadOnlyStringSpan` overload.
//
// spec §22.6 (v0.344): "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit … the call is a
// compile error with no candidates cited."
//
// ─── The defect ────────────────────────────────────────────────────────────
// The arm's guard was the LOWERED OPERAND'S SHAPE alone:
//
//     if (!strVal->getType()->isPointerTy() && !isStringSpanValue(strVal))
//         return nullptr;
//
// A `string` lowers to a POINTER, and so does a CLASS INSTANCE. So the shape
// test admits a class instance, and the arm then handed it to
// `parseWindow`/`loadStringData`, which read the OBJECT'S OWN BYTES as a
// `bz.string` descriptor ({ptr data, i64 len, i8 width}) and parsed the
// bytes it found there as TEXT.
//
// MEASURED on the base arm (the staging tip this lane started from), with
// `var o := new C2();` and `var v: int := 7;`:
//
//     int.TryParse(o, out v)   COMPILED, printed "False" / "0"
//
// The read is of memory the program does not own — and it is SILENT: no
// diagnostic, no abort, and the `out` slot received the failure default 0,
// exactly as it would for a well-formed input that failed to parse.
//
// The sibling arms of the same family do NOT have this hole, and their
// guards are the ones this arm now matches:
//
//   * the three-argument `TryParse(s, radix, out result)` form checks the
//     STATIC TYPE (stdlib §9.1 declares no span overload of the radix form);
//   * the two-argument `Parse(s, radix)` form checks the STATIC TYPE too.
//
// The STATIC type alone is not enough (the span overload's declared type is
// not the primitive `string`), and the SHAPE alone is not enough (a class
// instance lowers to a pointer). The two together are exact.
//
// ─── The remedy ────────────────────────────────────────────────────────────
// GUARD-AND-DROP, not a new refusal: every shape the guard touches is an
// ILLEGAL program (stdlib §9.1 declares `s: string`), and a refusal would be
// a NEW REJECTION of a program that compiles today. The call is dropped the
// way the compiler already drops a call it cannot lower.
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The load-bearing observable is the `out` SLOT. It is pre-set to 7, so the
// two behaviours are distinguishable:
//
//   * a call that RAN and failed writes the failure default (0) into it
//     (stdlib §9.3: "TryParse returns false in every case where Parse would
//     raise"; the out param gets the default) — this is the BASE arm, and it
//     is the arm that READ THE OBJECT'S BYTES;
//   * a DROPPED call never writes it, so it keeps 7 — this is the FIXED arm.
//
// The compiler's outcome is asserted as well (`ran`, exit code 0, and no IR
// verifier complaint), so a row can never pass on a crash.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// A `TryParse(s, out result)` program whose FIRST argument is `first` —
// the operand stdlib §9.1 declares as `s: string`. The `out` slot starts at
// 7, so a DROPPED call (the slot keeps 7) is distinguishable from a call
// that ran and failed (the slot receives the default 0).
std::string tryParseProg(const std::string& receiver, const std::string& first,
                          const std::string& decl) {
    return "module M;\n"
           "class C2\n"
           "  public F: int := 3;\n"
           "end C2;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var o := new C2();\n" + decl +
           "    var v: int := 7;\n"
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

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand.
// ---------------------------------------------------------------------------

// `int.TryParse(o, out v)` with `o` a class instance. On the base arm the
// arm read the object's own bytes as text and the `out` slot received the
// failure default 0. With the guard, the call is DROPPED and the slot keeps
// its pre-set value.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           class_instance_first_argument_is_not_parsed_as_text) {
    JitResult r = runAot(tryParseProg("int", "o", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}

// The same hole in the `long` member of the tower — the arm is shared, and
// the row pins that the guard is not `int`-specific.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           long_class_instance_first_argument_is_not_parsed_as_text) {
    std::string prog =
        "module M;\n"
        "class C2\n"
        "  public F: int := 3;\n"
        "end C2;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var o := new C2();\n"
        "    var v: long := 7;\n"
        "    var ok: bool := long.TryParse(o, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(prog);
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the guard must not swallow a legal call, and must not
// change the shapes that were already dropped.
// ---------------------------------------------------------------------------

// The POSITIVE CONTROL: a `string` first argument still parses.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           string_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg("int", "\"255\"", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n255\n"));
}

// The span overload (stdlib §9.1's `ReadOnlyStringSpan` form) still parses —
// the guard admits the {ptr,i32,i32} record by its shape.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           span_first_argument_still_parses) {
    JitResult r = runAot(tryParseProg(
        "int", "sp", "    var sp := new ReadOnlyStringSpan(\"255\", 0, 3);\n"));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n255\n"));
}

// An `int` first argument was already dropped (the shape test rejected it),
// and it stays dropped: the `out` slot is never written, so it keeps its
// pre-set value on BOTH arms — this row is a CONTROL, not the pin.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           an_int_first_argument_is_still_dropped) {
    JitResult r = runAot(tryParseProg("int", "16", ""));
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}

// The three-argument radix form (stdlib §9.1 declares NO span overload of
// it) already refused a class instance on the STATIC type, so it dropped
// the call on both arms. This row pins that the two arms of the family
// AGREE.
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           the_radix_form_also_drops_a_class_instance) {
    JitResult r = runAot(
        "module M;\n"
        "class C2\n"
        "  public F: int := 3;\n"
        "end C2;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var o := new C2();\n"
        "    var v: int := 7;\n"
        "    var ok: bool := int.TryParse(o, 16, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectCompiledWithoutIRFailure(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n7\n"));
}
