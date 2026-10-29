// Spec-derived regression test for issue #6647 — `Math.Abs` (and the
// `Floor`/`Ceiling`/`Truncate` family) at a §7.4-NARROWED `decimal?`.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.345) ─────────────────────
//
// spec §7.4 (v0.345) (docs/Bezel_Language_Spec.md line 909):
//   "…and from a declaration whose initializer is such an `expr` — `var x: T? :=
//   expr;` or `const x: T? := expr;` (§21.1) — which is the declaration's
//   assignment…"
//
// spec §22.6 (v0.345) (docs/Bezel_Language_Spec.md line 4591):
//   "**The argument's static type** is its type at the call: a read of a binding
//   §7.4 has narrowed non-null is of its non-nullable type…"
//
// spec §5.1 (v0.345) (docs/Bezel_Language_Spec.md line 649):
//   "Each implicit conversion of this section also **lifts** to the nullable
//   forms: `S?` implicitly converts to `T?` whenever `S` implicitly converts to
//   `T`"
//   => a `decimal` is NEVER implicitly converted to an integer or to a binary
//      float, so a `decimal` RESULT can only be consumed as a `decimal`.
//
// stdlib §2.2 (`Math`):
//   "Abs(x: int): int", "Abs(x: long): long", "Abs(x: double): double",
//   "Abs(x: decimal): decimal" — the `decimal` form's RESULT is a `decimal`,
//   and `Floor`/`Ceiling`/`Truncate` declare a `decimal` form too.
//
// ─── WHAT THE DEFECT WAS ────────────────────────────────────────────────────
//
// A `decimal` is a STRUCT (stdlib §4.4), not a binary float, so the §7.4
// narrowed `decimal?` payload has to be carried as a `decimal` all the way
// out. Two failures were measured on the `Math`/`Abs` lowering:
//
// (1) the compiler ABORTED (malformed IR):
//       var m: decimal? := 2.5m;
//       Console.WriteLine(Math.Abs(m));
//         "Both operands to ICmp instruction are not of the same type!"
//         "bezelc: codegen error: IR verification failed (malformed IR
//          emitted)"
//
// (2) and — once the payload was extracted — the call's RESULT TYPE was not
//     `decimal`, so the consumer read the `decimal` struct as an `int` and
//     printed the struct's low word: 25 for `2.5m` (the decimal's scaled
//     mantissa), not 2.5. That is the SILENT WRONG VALUE §5.1 forbids: a
//     `decimal` is never implicitly converted to an integer.
//
// ─── THE FIX ─────────────────────────────────────────────────────────────────
//
// The `Math` lowering now FAILS CLOSED when it produces a `decimal` RESULT for
// a call that is not typed `decimal`: it reports a located, spec-cited
// diagnostic instead of emitting a value the consumer will read as another
// primitive (and instead of aborting the compiler).
//
// THE REMEDY WAS NOT IN THIS FILE, AND IT HAS NOW LANDED (#7460, v0.350).
// The call's result type is the SEMANTIC layer's
// (src/sema/ExprTypecheck_Calls.cpp): its polymorphic-refinement block copied
// the operand's NULLABLE type to the result, and its #6297 `decimal` block
// required a NON-nullable `decimal` operand, so a narrowed `decimal?` operand
// kept the `int` placeholder. Both now read the operand's static type AT THE
// CALL (spec §22.6: a narrowed read is of its non-nullable type), the call is
// typed `decimal`, the fail-closed guard stands down, and the two narrowed
// rows below flipped from "refused" to the `decimal` the call denotes.
//
// The rows below pin: (a) the `decimal` payload is never read as another
// primitive, (b) the compiler never aborts on these shapes, (c) the LEGAL
// `decimal` forms still compute the `decimal` they declare, and (d) the
// UN-narrowed spelling is still refused by §7. (The fail-closed guard itself
// stays in the `Math` lowering for any other call it types non-`decimal`.)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// The NARROWED `decimal?` operand. ADMITTED (spec §22.6: a narrowed read is of
// its non-nullable type), typed `decimal`, and it prints the `decimal` — never
// a compiler abort, and never a value read as another primitive. (These two
// rows pinned a located REFUSAL until #7460 fixed the result type; they flipped
// with it.)
// ===========================================================================

BEZEL_TEST(math_abs_decimal_nullable_6647,
           narrowed_decimal_abs_computes_the_decimal_2_2_5_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal? := 2.5m; "
                        "    Console.WriteLine(Math.Abs(m));")),
                    "2.5");
}

// The same at the `Floor` position — the family that declares a `decimal`
// form too (stdlib §2.2 `Floor(x: decimal): decimal`): 2.5m floors to 2.
BEZEL_TEST(math_abs_decimal_nullable_6647,
           narrowed_decimal_floor_computes_the_decimal_2_2_5_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal? := 2.5m; "
                        "    if m <> null then "
                        "      Console.WriteLine(Math.Floor(m)); "
                        "    end;")),
                    "2");
}

// ===========================================================================
// The CONTROL. The LEGAL `decimal` forms still compute the `decimal` they
// declare — the guard does not fire on them.
// ===========================================================================

// stdlib §2.2 `Abs(x: decimal): decimal` — 2.5m is 2.5m.
BEZEL_TEST(math_abs_decimal_nullable_6647,
           plain_decimal_abs_computes_the_decimal_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal := 2.5m; "
                        "    Console.WriteLine(Math.Abs(m));")),
                    "2.5");
}

// stdlib §2.2 `Floor(x: decimal): decimal` — 2.5m floors to 2.
BEZEL_TEST(math_abs_decimal_nullable_6647,
           plain_decimal_floor_computes_the_decimal_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal := 2.5m; "
                        "    Console.WriteLine(Math.Floor(m));")),
                    "2");
}

// The `Convert` arm's result is a `string`, so a narrowed `decimal?` operand
// there is rendered as the decimal it denotes — the guard is scoped to a
// `decimal` RESULT.
BEZEL_TEST(math_abs_decimal_nullable_6647,
           narrowed_decimal_to_string_renders_the_decimal_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var m: decimal? := 2.5m; "
                        "    if m <> null then "
                        "      Console.WriteLine(Convert.ToString(m)); "
                        "    end;")),
                    "2.5");
}

// ===========================================================================
// The CONTROL. The UN-narrowed spelling is still refused by §7 — the §7 gate
// owns that position and its wording is unchanged.
// ===========================================================================

BEZEL_TEST(math_abs_decimal_nullable_6647,
           unnarrowed_decimal_abs_is_refused_7_5_1) {
    auto r = runAot(prog(
        "    var m: decimal? := null; "
        "    Console.WriteLine(Math.Abs(m));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Math.Abs` is nullable but the target type is "
           "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}
