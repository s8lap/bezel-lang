// fix #4501 — `=` / `<>` (and the relational and arithmetic operators) between
// operands of UNRELATED types must be rejected by sema, not reach codegen
// (§17.3 *The `=` and `<>` operators*, §8.4 *`=` and `<>`*, §5.1 *Implicit
// conversions*, §6.2 *Mixed-type arithmetic*, v0.306).
//
// THE RULE, established from the documents before the check was written:
//
//   * §17.3: "`=` and `<>` invoke `Equals` with a null-safe wrapper (§8.4)."
//   * §8.4: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper."
//     `string` has no `Equals(int)`; `bool` has no `Equals(int)`. The
//     candidate set is empty unless §5.1 supplies a conversion that makes one
//     applicable.
//   * §5.1's table has no row between `string` and any numeric type, none out
//     of `bool` at all ("Between integers and `bool`" is on its never-implicit
//     list), and none from a numeric type to `string`.
//   * §6.2 gives the disposition for the relational/arithmetic arms: "If no
//     implicit conversion exists between the operand types … the operation is
//     a compile error and an explicit cast is required."
//
// So the documents settle it: no applicable `Equals`, no conversion, compile
// error — and none of it should reach codegen.
//
// WHAT WENT WRONG. Measured on 16e992f0 (docs v0.306), one compile + one run
// each:
//   `string = 0`     -> reached codegen; `icmp eq ptr %2, i32 0` plus five more
//                       malformed instructions -> "IR verification failed". On
//                       an assertions-enabled LLVM that same IR aborts the
//                       compiler inside `ICmpInst::AssertOK` (SIGABRT, exit
//                       134, core dumped) — the symptom #4501 reports.
//   `bool = "x"`     -> the same shape.
//   `decimal = "x"`  -> "SExt only operates on integer".
//   `bool = 0`       -> compiled with NO diagnostic and the program printed
//                       `True`. `false = 0` answered `True` is not a claim the
//                       source makes: the worst of the four.
//
// ROOT CAUSE, and why the fix is a UNIFICATION. `typeBinary` carried TWO
// copies of one question — "does §5.1 admit a conversion between these operand
// types?" — one in the equality/relational arm (#1204) and one in the §6.2
// arithmetic-promotion arm. Both made `isPromotablePrimitive(lt) &&
// isPromotablePrimitive(rt)` a PRECONDITION OF REJECTING, so any pair that was
// not numeric-vs-numeric fell through the check and was silently admitted —
// the rule inverted. The two copies are now one call
// (`reportNoImplicitConversionBetweenOperands`), and the hole is closed once
// rather than twice. A second, independent reason `string = 0` and `bool = 0`
// were admitted: the §5.1 constant-narrowing escape asked only "is either side
// literal-derived?", so the `0` exempted the pair by itself. The escape is now
// keyed on the OTHER operand's type, because a numeric constant can only adopt
// a type a numeric constant can have (§5.1) — never `bool`, never `string`.
//
// RELATION TO #4500 (the preceding commit). Both issues are the same question
// — is this operand admissible here — asked at two different grains. #4500 is
// §3.5's PER-OPERAND table ("arithmetic on the numeric types"), which is why it
// catches `bool * bool`, where the operand types match. This one is §5.1's
// PER-PAIR conversion rule, which is why it catches `bool = 0`, where each
// operand is individually fine for `=` (§3.5 gives equality to every
// primitive) and only the pairing is not.
//
// ⚠ This is a REJECTION, so the false-reject hazard dominates; T4/T5/T6 are
// the accept-controls and the unchanged-wording pins.
//
// TEST MATRIX:
//   T1  the issue's four measured rows through the REAL driver (`runAot`):
//       rejected with the §17.3/§8.4 wording, and demonstrably NOT by an
//       IR-verifier failure, crash, signal or assertion — the pre-fix symptom
//       for three of them. (`failedAtCompileTimeWithRealLocation` is
//       deliberately NOT used: it passes on a crash's own backstop
//       diagnostic, which is exactly what these rows produced.)
//   T2  both operand orders, and `<>` as well as `=`.
//   T3  the relational and arithmetic arms reached through the SAME call:
//       `string < 0` and `string * int` (the latter died in the IR verifier).
//   T4  ACCEPT-CONTROLS that RUN and assert values: same-type equality, §5.1
//       widening pairs, `char = int`, the constant-narrowing escape
//       (`ulong > 1000`, `decimal = 2`), `s = null`, and `Object = string`.
//   T5  `Object` on either side is NOT rejected — §5.1's reference conversions
//       include "any type to `Object`" (boxing, §8.6), so `o = 5` is
//       admissible under §8.4 and rejecting it would be a false reject with no
//       authority. (Its codegen is separately defective; sema must stay out.)
//   T6  ONE mixed-numeric wording now, not two: the ARITHMETIC arm's text is
//       unchanged byte-for-byte (several lit rows pin it) and the equality/
//       relational arm, which carried its own near-copy ending
//       "(§6.2/§5.1)", now emits that same text — nothing pinned the old
//       spelling. fix #4512 (§5.1, v0.306, a later commit on this same
//       call) closed the one remaining asymmetry this unification had
//       deliberately preserved: the arithmetic arm now has the SAME §5.1
//       constant-narrowing escape as the relational/equality arm
//       (`uint * 3`, `ulong + 1000` now admit, matching `uint > 1000`); a
//       non-literal operand of the same value still has no §5.1 relation
//       and stays rejected.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::parseAndCollect;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Sema only — nothing is compiled or run, so an assertion on the diagnostic
// text here can never be satisfied by a crash. Used for the breadth rows.
bezel::test::SemaResult semaAll4501(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag4501(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr std::string_view kHead = "has no implicit conversion between operand";
constexpr std::string_view kEqualsReason =
    "`=`/`<>` invoke `Equals` (\xC2\xA7" "17.3/\xC2\xA7" "8.4)";
constexpr std::string_view kNonEqReason =
    "table has no row between them, so the operation is a compile error";
// The pre-existing mixed-numeric tail, pinned by several lit rows.
constexpr std::string_view kNumericTail =
    "(\xC2\xA7" "6.2); add an explicit cast";

std::string prog4501(std::string_view body) {
    return std::string("module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

// A driver-level rejection row. The pre-fix behaviour for three of the four T1
// rows was a codegen death (an IR-verifier failure here; a SIGABRT inside
// LLVM's `ICmpInst::AssertOK` on an assertions-enabled build), so "rejected
// correctly" is separated from "died" by explicit negative assertions rather
// than by a helper that a crash's own backstop diagnostic would satisfy.
void expectDriverRejectsPair(const std::string& body, const char* op,
                             const char* lty, const char* rty,
                             bool equality) {
    auto r = runAot(prog4501(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("operator `") + op +
            "` has no implicit conversion between operand types `" + lty +
            "` and `" + rty + "`")
               .c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, std::string(equality ? kEqualsReason : kNonEqReason).c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "SExt only operates"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "ICmpInst"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's four measured rows, through the real driver.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t1_string_eq_int_rejected_spec_17_3) {
    // Pre-fix: `icmp eq ptr, i32` -> IR verification failed (SIGABRT in
    // `ICmpInst::AssertOK` on an assertions-enabled LLVM).
    expectDriverRejectsPair(
        " var s: string := \"x\"; "
        " Console.WriteLine((s = 0).ToString()); return 0; ",
        "=", "string", "int", /*equality=*/true);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t1_bool_eq_string_rejected_spec_17_3) {
    // Pre-fix: the same malformed `icmp`.
    expectDriverRejectsPair(
        " var b: bool := true; "
        " Console.WriteLine((b = \"x\").ToString()); return 0; ",
        "=", "bool", "string", /*equality=*/true);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t1_bool_eq_int_rejected_spec_17_3) {
    // Pre-fix: compiled clean and the program printed `True` — `false = 0`
    // answered `True`. Two independent holes admitted this one: the
    // `isPromotablePrimitive` precondition AND the unkeyed literal escape.
    expectDriverRejectsPair(
        " var b: bool := false; "
        " Console.WriteLine((b = 0).ToString()); return 0; ",
        "=", "bool", "int", /*equality=*/true);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t1_decimal_eq_string_rejected_spec_17_3) {
    // Pre-fix: "SExt only operates on integer" — a codegen error, not a
    // diagnostic.
    expectDriverRejectsPair(
        " var d: decimal := 2m; "
        " Console.WriteLine((d = \"x\").ToString()); return 0; ",
        "=", "decimal", "string", /*equality=*/true);
}

// ===========================================================================
// T2 — both operand orders, and `<>` beside `=`. The predicate is symmetric;
// the pre-fix hole was too, so an order-blind matrix is the honest one.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t2_both_orders_and_both_equality_operators_spec_8_4) {
    struct Row { const char* decls; const char* lhs; const char* rhs; };
    const Row rows[] = {
        {" var s: string := \"x\"; var i: int := 0; ", "s", "i"},
        {" var b: bool := true; var i: int := 0; ", "b", "i"},
        {" var b: bool := true; var s: string := \"x\"; ", "b", "s"},
        {" var d: decimal := 2m; var s: string := \"x\"; ", "d", "s"},
        {" var c: char := 'a'; var s: string := \"x\"; ", "c", "s"},
    };
    for (const Row& row : rows) {
        for (std::string_view op : {"=", "<>"}) {
            for (int swap = 0; swap < 2; ++swap) {
                const char* l = swap ? row.rhs : row.lhs;
                const char* r2 = swap ? row.lhs : row.rhs;
                auto r = semaAll4501(prog4501(
                    std::string(row.decls) + " if " + l + " " +
                    std::string(op) + " " + r2 + " then return 1; end; "
                    " return 0; "));
                BEZEL_EXPECT_TRUE(hasDiag4501(r, kHead));
                BEZEL_EXPECT_TRUE(hasDiag4501(r, kEqualsReason));
            }
        }
    }
}

// ===========================================================================
// T3 — the relational and arithmetic arms reach the SAME call. That is the
// point of the unification: one hole, closed once.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t3_relational_unrelated_pair_rejected_spec_6_2) {
    auto r = semaAll4501(prog4501(
        " var s: string := \"x\"; if s < 0 then return 1; end; return 0; "));
    BEZEL_EXPECT_TRUE(hasDiag4501(r, kHead));
    BEZEL_EXPECT_TRUE(hasDiag4501(r, kNonEqReason));
    // Not given the equality authority — the arms share the call, not the text.
    BEZEL_EXPECT_FALSE(hasDiag4501(r, kEqualsReason));
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t3_arithmetic_unrelated_pair_rejected_spec_6_2) {
    // Measured pre-fix: `string * int` reached codegen and died in LLVM's IR
    // verifier. The arithmetic arm inherits the corrected predicate.
    auto r = runAot(prog4501(
        " var s: string := \"x\"; var i: int := 2; "
        " Console.WriteLine((s * i).ToString()); return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` has no implicit conversion between operand types "
           "`string` and `int`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

// ===========================================================================
// T4 — ACCEPT-CONTROLS. Each RUNS and asserts a value.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t4_same_type_and_widening_equality_still_computes_spec_5_1) {
    auto r = runAotChecked(prog4501(
        " var a: string := \"x\"; var b: string := \"x\"; "
        " var i: int := 5; var l: long := 5; var c: char := 'A'; "
        " var d: decimal := 2m; var f: float := 1.5f; var g: double := 1.5; "
        " if a <> b then return 1; end; "          // string = string
        " if i <> l then return 2; end; "          // §5.1 int -> long
        " if c <> 65 then return 3; end; "         // §5.1 char -> int
        " if d <> 2 then return 4; end; "          // §5.1 constant narrowing
        " if f <> g then return 5; end; "          // §5.1 float -> double
        " return 42; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t4_constant_narrowing_escape_preserved_spec_5_1) {
    // #1204's escape: an untyped constant adopts the other operand's type, so
    // these compile without a cast. Keying the escape on the other operand
    // must not have disturbed them.
    auto r = runAotChecked(prog4501(
        " var u: ulong := 3000000000; var v: uint := 7; "
        " if u < 1000 then return 1; end; "
        " if v > 1000 then return 2; end; "
        // fix #6868 (§5.1, v0.340): this row used `0 - 5` (-5, outside
        // ulong's range), which constant narrowing does not admit -- "a
        // constant whose value does not fit the target's range is a compile
        // error". An in-range literal-derived expression keeps the escape.
        " if u < 7 - 5 then return 3; end; "
        " return 42; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t4_null_and_boxed_equality_still_computes_spec_8_4) {
    // §8.4's null-safe wrapper and §5.1's "any type to `Object`" boxing.
    auto r = runAotChecked(prog4501(
        " var s: string := \"x\"; var o: Object := \"x\"; "
        " if s = null then return 1; end; "
        " if o <> s then return 2; end; "
        " return 42; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(sema_binary_operand_pair_4501,
           t4_bool_equality_still_admitted_spec_3_5) {
    // §3.5 gives equality to every primitive, `bool` included. Only the
    // PAIRING is rejected, never `bool = bool`.
    auto r = runAotChecked(prog4501(
        " var a: bool := true; var b: bool := false; "
        " if a = b then return 1; end; if a <> b then return 42; end; "
        " return 2; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T5 — `Object` on either side is NOT rejected by sema. §5.1's reference
// conversions include "any type to `Object`" (boxing, §8.6), so `o = 5` has an
// applicable `Object.Equals(Object?)` (§8.1) and rejecting it here would be a
// false reject with no authority. Sema-only, because that row's CODEGEN is
// separately defective (its own issue) — this pins the sema verdict, which is
// what the fix owns.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t5_object_operand_not_rejected_by_this_rule_spec_5_1) {
    for (const char* body :
         {" var o: Object := 5; var i: int := 5; "
          " if o = i then return 1; end; return 0; ",
          " var o: Object := 5; var i: int := 5; "
          " if i = o then return 1; end; return 0; ",
          " var o: Object := \"x\"; var s: string := \"x\"; "
          " if o <> s then return 1; end; return 0; "}) {
        auto r = semaAll4501(prog4501(body));
        BEZEL_EXPECT_FALSE(hasDiag4501(r, kHead));
    }
}

// ===========================================================================
// T6 — ONE mixed-numeric wording now, not two. The arithmetic arm's text is
// unchanged byte-for-byte (several lit rows pin it); the equality/relational
// arm, which had its own near-copy ending "(§6.2/§5.1)", now emits that same
// text — nothing pinned the old spelling (the #1204 gtest rows assert
// `hasErrors()` only), and two wordings for one rule was half the defect.
// Without this row a later tidy-up could change the wording silently.
// ===========================================================================
BEZEL_TEST(sema_binary_operand_pair_4501,
           t6_mixed_numeric_wording_unchanged_spec_6_2) {
    for (const char* body :
         {" var a: double := 1.0; var b: decimal := 2m; "
          " var x := a + b; return 0; ",
          " var a: float := 1.0f; var b: decimal := 2m; "
          " var x := a * b; return 0; ",
          " var a: int := 1; var b: uint := 2; "
          " if a < b then return 1; end; return 0; "}) {
        auto r = semaAll4501(prog4501(body));
        BEZEL_EXPECT_TRUE(hasDiag4501(r, kHead));
        BEZEL_EXPECT_TRUE(hasDiag4501(r, kNumericTail));
        BEZEL_EXPECT_FALSE(hasDiag4501(r, kEqualsReason));
    }
}

// fix #4512 (§5.1, v0.306): supersedes
// `t6_arithmetic_arm_has_no_literal_escape_spec_6_2` (this exact name),
// which pinned the OLD asymmetry as deliberate — "the arithmetic arm has
// never had #1204's escape" — and is now the wrong verdict: §5.1's
// constant-narrowing paragraph "applies at every position where this
// section's implicit conversions apply — there is no position carve-out",
// and §6.2 explicitly promotes "using §5.1", so `ulong + 1000` has the same
// authority as `ulong > 1000` (#1204). Runs and asserts the VALUE, not just
// acceptance: the escape adopts the literal into the OTHER operand's type
// (§5.1), so `u * 3` computes in `uint` and `v + 100` in `ulong` — neither
// widens to/from `int` (§5.1's signed/unsigned "never implicit" pairs), so
// only the escape admits them.
BEZEL_TEST(sema_binary_operand_pair_4501,
           t6_arithmetic_arm_literal_escape_admitted_spec_5_1) {
    auto r = runAotChecked(prog4501(
        " var u: uint := 5; var x := u * 3; "
        " var v: ulong := 5; var y := v + 100; "
        " return int(x) + int(y); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15 + 105);
}

// The escape is keyed on the operand being literal-DERIVED (§5.1's
// "compile-time constant"); a typed VARIABLE of the same value has no §5.1
// relation to a mismatched-signedness or narrower-width operand and stays
// rejected — the escape widens acceptance for CONSTANTS only, exactly as
// #1204 already established for the relational arm (`uint > n` where `n` is
// a variable stays rejected too).
BEZEL_TEST(sema_binary_operand_pair_4501,
           t6_arithmetic_arm_non_literal_pair_still_rejected_spec_5_1) {
    for (const char* body :
         {" var u: uint := 5; var n: int := 3; "
          " var x := u * n; return 0; ",
          " var v: ulong := 5; var m: int := 100; "
          " var y := v + m; return 0; "}) {
        auto r = semaAll4501(prog4501(body));
        BEZEL_EXPECT_TRUE(hasDiag4501(r, kHead));
        BEZEL_EXPECT_TRUE(hasDiag4501(r, kNumericTail));
    }
}
