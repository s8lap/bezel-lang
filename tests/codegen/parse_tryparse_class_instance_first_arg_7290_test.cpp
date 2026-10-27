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
// The #7290 lane landed GUARD-AND-DROP: every shape the guard touches is an
// ILLEGAL program (stdlib §9.1 declares `s: string`), and a refusal would be
// a NEW REJECTION of a program that compiles today, so the call was dropped
// the way the compiler already drops a call it cannot lower.
//
// ─── The fix-forward (campaign 2's anchor) ─────────────────────────────────
// The drop was the wrong answer, and §22.6 (v0.344) says so: "If no
// candidate is applicable at all … the call is a compile error with no
// candidates cited, because no overload was reachable." A DROPPED call is
// not a compile error — the program LINKS, RUNS, and answers a wrong value
// (`false` / `0`) for a program the standard library makes a compile error.
// The anchor's ONE admission gate resolves the call against the DECLARED
// surface and REFUSES it with a located §22.6 diagnostic, so the four pins
// below now assert the refusal (and the two CONTROL rows — a `string` first
// argument and the `ReadOnlyStringSpan` overload — still compile and run).
//
// ─── What the rows assert ──────────────────────────────────────────────────
// The load-bearing observable is the `out` SLOT. It is pre-set to 7, so the
// two behaviours are distinguishable:
//
//   * a call that RAN and failed writes the failure default (0) into it
//     (stdlib §1's Try convention, shape (c): "assigns `default(T)`"; and
//     §9.1's own inline comment, "// shape (c): false (out = 0)") — this is
//     the BASE arm, and it
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
using bezel::test::failedAtCompileTimeWithRealLocation;
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

// The §22.6 ADMISSION refusal: the named diagnostic, at a real location, and
// the compiler did not merely abort.
//
// fix-forward (campaign 2's anchor): this file's four pins asserted the
// GUARD-AND-DROP semantics the #7290 lane landed ("the call is dropped the
// way the compiler already drops a call it cannot lower"). The spec says
// otherwise: stdlib §9.1 declares `TryParse(s: string, out result: int)`
// (and the `ReadOnlyStringSpan` overload), so a class instance or an `int`
// reaches NO candidate, and §22.6 (v0.344) makes that "a compile error with
// no candidates cited, because no overload was reachable". The anchor's ONE
// admission gate now refuses those shapes with a located diagnostic, so the
// four rows below assert the refusal instead of the drop.
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: a class instance is not a parseable operand.
// ---------------------------------------------------------------------------

// `int.TryParse(o, out v)` with `o` a class instance. On the base arm the
// arm read the object's own bytes as text and the `out` slot received the
// failure default 0. The #7290 lane's guard made the call DROPPED; the
// anchor's §22.6 admission gate makes it a located COMPILE ERROR, which is
// what the spec asks for (stdlib §9.1 declares no class-instance overload,
// so no candidate is reachable).
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           class_instance_first_argument_is_not_parsed_as_text) {
    JitResult r = runAot(tryParseProg("int", "o", ""));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

// The same hole in the `long` member of the tower — the arm is shared, and
// the row pins that the refusal is not `int`-specific.
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
    expectRefused(r, "no matching overload of `long.TryParse`");
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

// An `int` first argument was already dropped (the shape test rejected it)
// and the #7290 lane's row pinned that drop. §22.6 (v0.344) says a call
// that reaches no candidate "is a compile error with no candidates cited",
// so the anchor refuses it — the row now asserts the refusal. (RENAMED from
// `an_int_first_argument_is_still_dropped`.)
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           an_int_first_argument_is_refused) {
    JitResult r = runAot(tryParseProg("int", "16", ""));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

// The three-argument radix form (stdlib §9.1 declares NO span overload of
// it) already refused a class instance on the STATIC type, so it dropped
// the call on both arms. The anchor refuses it with the same §22.6
// diagnostic, so the two arms of the family AGREE. (RENAMED from
// `the_radix_form_also_drops_a_class_instance`.)
BEZEL_TEST(parse_tryparse_class_instance_first_arg_7290,
           the_radix_form_also_refuses_a_class_instance) {
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
    expectRefused(r, "no matching overload of `int.TryParse`");
}
