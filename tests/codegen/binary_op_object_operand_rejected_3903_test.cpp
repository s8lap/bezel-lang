// Regression tests for #3903 (§3.1/§3.4/§3.5/§8.1/§25/§26.3, v0.294):
// `Object + int` (no matching operator) reached codegen and failed LLVM's
// IR verifier instead of a clean sema rejection.
//
// Authority:
//   §3.1 *Type kinds* — "Every type derives from `Object`" lists `Object`
//   apart from the enumerated "primitive value types".
//   §3.4 *Keyword aliases for built-in types* — "`Object` is a class and is
//   referenced as `Object` unconditionally — there is no `object` spelling."
//   §3.5 *Operators on primitive types* — enumerates the intrinsic
//   arithmetic/comparison/bitwise/shift operators over an explicit list of
//   primitive value types (`byte`..`decimal`, `bool`, `char`, `string`);
//   `Object` is not on that list.
//   §8.1 *Definition* — `Object`'s complete member surface is `GetType`,
//   `Equals`, `GetHashCode`, `ToString`, `ReferenceEquals`; "User code cannot
//   redeclare or replace `Object`."
//   §25 *Operator Overloading* / §25.1 — an operator is a `public static`
//   method a type declares in its own body.
//   §26.3 *Restrictions* (extension blocks) — "No operator overloads."
//
// THE DEFECT — measured on HEAD before this fix (`bezelc -o /tmp/x prog.bzl`):
//
//     module M; class C
//       public static method Main(): int
//         var o: Object := i;
//         var dummy := o + 1;
//         ...
//
//   produced:
//     Both operands to a binary operator are not of the same type!
//       %8 = add ptr %7, i32 1
//     bezelc: codegen error: IR verification failed (malformed IR emitted...)
//
// Root cause: the implementation represents an `Object`-typed operand as
// `TypeRef::Kind::Primitive`/`PrimitiveType::Object` (so boxing/`is`/`as` can
// treat it uniformly with the true primitives). Every existing "operator
// requires a declared overload" gate in `typeBinary`
// (`src/sema/ExprTypecheck_Exprs.cpp`) keyed off `TypeRef::Kind::Named` (the
// record/class/interface/choice checks) or an enumerated primitive kind (the
// bitwise/`mod`/nullable checks) — `Object` matched neither, so every
// operator family fell through to codegen typed as an ordinary primitive:
//   * `+ - * / mod` died in LLVM's IR verifier (an add/sub/mul/sdiv/srem
//     between a `ptr` and an `i32`).
//   * `& | ^` didn't even crash — they hit the unrelated #2434 lower-to-zero
//     backstop and silently compiled to a WRONG value (a zero-initialized
//     slot, exit 0, only an "internal:" stderr note).
//   * `< <= > >=` with a non-`Object` other operand died in IR verification
//     the same way as arithmetic; `Object > Object` (same `PrimitiveType` on
//     both sides, so the generic "no implicit conversion" gate never fired)
//     compiled CLEANLY to a raw pointer `icmp` — an order over heap
//     addresses that violates §30.13's determinism guarantee, silently.
//
// This file pins the fix: every one of these operand shapes is now a clean
// sema rejection naming the operator and `Object`, at a real source
// location — never `hasErrors()`/`!ran` alone, which cannot distinguish a
// clean reject from the codegen crash / silent wrong-value outcomes above.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

namespace {

// True iff the diagnostics list carries a message containing every needle —
// mirrors binary_mod_bool_string_rejected_3243_test.cpp's helper.
bool diagnosticsMentionAll(const bezel::DiagnosticEngine& de,
                            std::initializer_list<const char*> needles) {
    for (const auto& d : de.diagnostics()) {
        bool allFound = true;
        for (const char* n : needles) {
            if (d.message.find(n) == std::string::npos) {
                allFound = false;
                break;
            }
        }
        if (allFound) return true;
    }
    return false;
}

// Compiles `body` inside a standard `Main(): int` wrapper and asserts a
// clean, real-location rejection naming `op` and `Object`, and that it never
// reaches LLVM's IR verifier (the crash this fix replaces) or the codegen
// error string it emits.
void expectObjectOperatorRejected(const std::string& body,
                                   const char* op) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(
        r.diagnostics,
        {(std::string("operator `") + op + "` is not defined for type "
          "`Object`").c_str(), "\xC2\xA7""25"}));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "codegen error"));
}

}  // namespace

// ── Arithmetic — Object on the LEFT ───────────────────────────────────────

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_plus_int_lhs) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o + 1; return 0;", "+");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_minus_object) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var p: Object := 3; var d := o - p; return 0;",
        "-");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_mul_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o * 2; return 0;", "*");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_div_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o / 2; return 0;", "/");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_mod_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o mod 2; return 0;", "mod");
}

// ── Arithmetic — Object on the RIGHT (was a DIFFERENT crash: "Call
//    parameter type does not match function signature", since the result
//    type followed the non-Object LEFT operand while codegen still fed the
//    boxed pointer into the intrinsic) ─────────────────────────────────────

BEZEL_TEST(binary_op_object_operand_rejected_3903, int_plus_object_rhs) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := 1 + o; return 0;", "+");
}

// ── Bitwise / shift — silently degraded to a WRONG VALUE pre-fix (#2434
//    backstop), not a crash; still the same missing-rejection defect ──────

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_and_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o & 2; return 0;", "&");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_or_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o | 2; return 0;", "|");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_xor_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o ^ 2; return 0;", "^");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_shl_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o << 2; return 0;", "<<");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_shr_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o >> 2; return 0;", ">>");
}

// ── Relational — `Object < int` etc. crashed like arithmetic ─────────────

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_lt_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o < 2; return 0;", "<");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_le_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o <= 2; return 0;", "<=");
}

BEZEL_TEST(binary_op_object_operand_rejected_3903, object_ge_int) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var d := o >= 2; return 0;", ">=");
}

// `Object > Object` is the SILENT case: pre-fix it compiled cleanly to a raw
// pointer `icmp` (no crash at all, both operands share one `PrimitiveType`)
// — a nondeterministic heap-address order with no diagnostic. This is the
// most important row in this file: a bare `!ran`/`hasErrors()` check would
// have reported this row as ALREADY PASSING pre-fix (compiled and ran with
// exit 0), which is exactly why the diagnostic-text assertion matters here.
BEZEL_TEST(binary_op_object_operand_rejected_3903, object_gt_object) {
    expectObjectOperatorRejected(
        "var o: Object := 5; var p: Object := 3; var d := o > p; return 0;",
        ">");
}

// ── Nullable Object operand ───────────────────────────────────────────────

BEZEL_TEST(binary_op_object_operand_rejected_3903, nullable_object_plus_int) {
    expectObjectOperatorRejected(
        "var o: Object? := 5; var d := o + 1; return 0;", "+");
}

// ── MUST-PASS controls: `=`/`<>` on Object are UNAFFECTED (§8.4: every
//    type gets `=`/`<>` via `Equals`; Object's inherited reference-equality
//    Equals answers them through ordinary dispatch, not this gate) ────────

BEZEL_TEST(binary_op_object_operand_rejected_3903,
           object_equals_reference_equality_still_works) {
    // Two distinct boxes of the same int value are NOT reference-equal.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 5; "
        "  var eq := o = p; "
        "  return (if eq then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(binary_op_object_operand_rejected_3903,
           object_equals_same_reference_still_works) {
    // The SAME box compares equal to itself.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var q: Object := o; "
        "  var eq := o = q; "
        "  return (if eq then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(binary_op_object_operand_rejected_3903,
           object_not_equals_still_works) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  var p: Object := 5; "
        "  var neq := o <> p; "
        "  return (if neq then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── MUST-PASS control: ordinary numeric arithmetic is unaffected ─────────

BEZEL_TEST(binary_op_object_operand_rejected_3903, int_plus_int_still_works) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := 3; "
        "  var b := 4; "
        "  return a + b; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
