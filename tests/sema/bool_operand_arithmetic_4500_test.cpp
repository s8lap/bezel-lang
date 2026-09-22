// fix #4500 — a binary ARITHMETIC operator (`+`, `-`, `*`, `/`) with a `bool`
// operand is ill-formed (§3.5 *Operators on primitive types*, §6.2 *Mixed-type
// arithmetic*, §5.1 *Implicit conversions*, v0.306).
//
// THE RULE, established from the documents before the check was written:
//
//   * §3.5 enumerates each intrinsic operator group BY OPERAND TYPE:
//     "**Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`,
//     `long`, `ulong`, `float`, `double`, `decimal`: `+`, `-`, `*`, `/`, unary
//     `-`, and unary `+`" — `bool` is absent — beside "**Ordering** (`<`, `<=`,
//     `>`, `>=`) on every primitive **except `bool`**". §3.5 then closes the
//     door: "The set of admitted combinations is fixed; primitives do not
//     declare cross-type `operator +` methods that user code could inspect."
//   * §5.1's implicit-conversion table has no row out of `bool`, and its
//     never-implicit list names "Between integers and `bool`" explicitly.
//   * §6.2 gives the disposition: "If no implicit conversion exists between the
//     operand types … the operation is a compile error and an explicit cast is
//     required."
//   * §4.1: `bool` has "No implicit conversion to or from any other type."
//
// So the documents settle it: ill-formed, and a compile error, with `int(b)`
// (§5.2) as the remedy — the same one §3.5 already names for the ordering arm.
//
// WHAT WENT WRONG. Measured on HEAD before the fix (bezelc @ 8fd7cabf, docs
// v0.306), one compile + one run each:
//   `decimal * bool` -> compiled with NO diagnostic; the program printed `-2`
//   `double * bool`  -> compiled with NO diagnostic; the program printed `-2`
//   `bool + bool`    -> compiled with NO diagnostic; printed `True`
//   `bool * bool`    -> compiled with NO diagnostic; printed `False`
//   `int * bool`     -> malformed IR: "Call parameter type does not match
//                       function signature", `@llvm.smul.with.overflow.i32(
//                       i32 %3, i1 %4)` -> "IR verification failed"
//   `int + bool`     -> the same IR-verifier failure
// One clean rejection replaces all six outcomes.
//
// THE FIX IS A UNIFICATION, NOT A NEW CHECK. §3.5 answers ONE question for
// both families — "does the intrinsic-operator table admit an operand of this
// type?" — and typeBinary already had exactly one site answering it for `bool`
// (the #1574 ordering gate). The arithmetic operators JOIN that site's operator
// set: same `isBool` predicate, same `if`, same `report` call, with only the
// reason clause selected by family. No second `bool`-operand check was added.
//
// ⚠ This is a REJECTION, so the false-reject hazard dominates; T4–T7 below are
// the accept-and-unchanged-wording controls and they outnumber the reject rows.
//
// TEST MATRIX:
//   T1  the issue's four measured rows, through the REAL driver (`runAot`):
//       rejected, with the §3.5 arithmetic wording, and demonstrably NOT by an
//       IR-verifier failure / crash / signal — the pre-fix symptom for two of
//       them. (`failedAtCompileTimeWithRealLocation` is deliberately NOT used:
//       it passes on a crash's own backstop diagnostic.)
//   T2  the whole arithmetic operator set `+ - * /`, `bool` on the LEFT, on the
//       RIGHT, and on BOTH — the gate is per-operand, not per-pair, so a
//       same-typed `bool * bool` is rejected too (it was not, and printed
//       `False`).
//   T3  every numeric partner type (`byte`…`decimal`, `char`) in both orders.
//   T4  ACCEPT-CONTROLS that RUN and assert a value: the same expressions with
//       the §5.2 remedy applied, ordinary mixed-numeric arithmetic, and `=`/
//       `<>`/`and`/`or`/`not` on `bool` — all untouched by the widening.
//   T5  the ORDERING arm's diagnostic is byte-for-byte unchanged (lit rows pin
//       its text): `b < c` still says "no ordering of `false`/`true`".
//   T6  `string + bool` still draws §4.6's CONCATENATION diagnostic, not this
//       one — §4.6 is the more specific authority for a string operand and its
//       remedy (interpolation / `.ToString()`) is the actionable one.
//   T7  `&`/`|`/`^`/`<<`/`>>` (§6.5) and `mod` (§6.1) keep their own `bool`
//       wording: those are different spec rules, already correct, not copies of
//       this one.

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

// Parse + collect + resolve + typecheck. Sema only — nothing is compiled or
// run, so an assertion on the diagnostic text here can never be satisfied by a
// crash. Used for the breadth matrices (T2/T3), where one forked compile+link
// per row would buy nothing the four T1 driver rows do not already establish.
bezel::test::SemaResult semaAll4500(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag4500(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The exact wording the fix emits. Asserting the TEXT — the shared §3.5 head
// AND the arithmetic-specific reason clause — is what makes this a contract on
// the diagnostic rather than on "something went wrong somewhere".
constexpr std::string_view kBoolHead = "is not defined for `bool` operands";
constexpr std::string_view kSec35 = "\xC2\xA7" "3.5";
constexpr std::string_view kArithReason =
    "defines arithmetic (`+`, `-`, `*`, `/`) on the numeric types only";
constexpr std::string_view kRemedy = "convert explicitly (`int(b)`)";
// The ordering arm's clause (T5) — unchanged by this fix.
constexpr std::string_view kOrderReason = "no ordering of `false`/`true` is";

std::string prog4500(std::string_view body) {
    return std::string("module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

// A driver-level (AOT) rejection row: the whole `bezelc` pipeline must refuse
// the program, naming THIS diagnostic, and must never reach codegen — the
// pre-fix symptom for `int * bool` / `int + bool` was an IR-verifier failure,
// and #4501's sibling rows abort inside LLVM outright on an assertions-enabled
// build, so "rejected correctly" is distinguished from "died" explicitly.
void expectDriverRejectsBoolArithmetic(const std::string& body,
                                       const char* op) {
    auto r = runAot(prog4500(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("operator `") + op + "` is not defined for `bool` "
                                             "operands")
               .c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kArithReason).c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kRemedy).c_str()));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's four measured rows, through the real driver.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t1_decimal_times_bool_rejected_spec_3_5) {
    // Pre-fix: compiled clean, program printed `-2`.
    expectDriverRejectsBoolArithmetic(
        " var d: decimal := 2m; var b: bool := true; "
        " Console.WriteLine((d * b).ToString()); return 0; ",
        "*");
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t1_double_times_bool_rejected_spec_3_5) {
    // Pre-fix: compiled clean, program printed `-2`.
    expectDriverRejectsBoolArithmetic(
        " var d: double := 2.0; var b: bool := true; "
        " Console.WriteLine((d * b).ToString()); return 0; ",
        "*");
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t1_int_times_bool_rejected_spec_3_5) {
    // Pre-fix: "Call parameter type does not match function signature!" on
    // `@llvm.smul.with.overflow.i32(i32, i1)` -> IR verification failed.
    expectDriverRejectsBoolArithmetic(
        " var i: int := 2; var b: bool := true; return i * b; ", "*");
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t1_int_plus_bool_rejected_spec_3_5) {
    // Pre-fix: the same IR-verifier failure on `sadd.with.overflow`.
    expectDriverRejectsBoolArithmetic(
        " var i: int := 2; var b: bool := true; return i + b; ", "+");
}

// ===========================================================================
// T2 — the whole arithmetic operator set, `bool` on either side and on both.
// §3.5's table is read PER OPERAND, so a same-typed `bool * bool` pair is
// rejected on exactly the same authority as the mixed one.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t2_all_arithmetic_operators_bool_right_rejected_spec_3_5) {
    for (std::string_view op : {"+", "-", "*", "/"}) {
        auto r = semaAll4500(prog4500(
            std::string(" var b: bool := true; return 5 ") + std::string(op) +
            " b; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kBoolHead));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kSec35));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kArithReason));
    }
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t2_all_arithmetic_operators_bool_left_rejected_spec_3_5) {
    for (std::string_view op : {"+", "-", "*", "/"}) {
        auto r = semaAll4500(prog4500(
            std::string(" var b: bool := true; return b ") + std::string(op) +
            " 5; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kBoolHead));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kArithReason));
    }
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t2_bool_op_bool_rejected_spec_3_5) {
    // Pre-fix `bool + bool` printed `True` and `bool * bool` printed `False`:
    // arithmetic answered in the boolean domain, with no diagnostic. A
    // pair-compatibility rule would never have caught this row (the operand
    // types match); §3.5's per-operand table does.
    for (std::string_view op : {"+", "-", "*", "/"}) {
        auto r = semaAll4500(prog4500(
            std::string(" var a: bool := true; var b: bool := false; "
                        " var x := a ") +
            std::string(op) + " b; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kBoolHead));
        BEZEL_EXPECT_TRUE(hasDiag4500(r, kArithReason));
    }
}

// ===========================================================================
// T3 — every numeric partner §3.5's arithmetic bullet lists, both orders.
// The defect's three symptoms were partner-dependent (silent wrong value for
// `decimal`/`double`, malformed IR for the integers), so the row set is the
// whole list rather than the two the issue happened to measure.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t3_every_numeric_partner_rejected_spec_3_5) {
    for (std::string_view ty : {"byte", "sbyte", "short", "ushort", "int",
                                "uint", "long", "ulong", "float", "double",
                                "decimal", "char"}) {
        const std::string decl =
            std::string(" var n: ") + std::string(ty) + " := default(" +
            std::string(ty) + "); var b: bool := true; ";
        auto rr = semaAll4500(prog4500(decl + " var x := n * b; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(rr, kBoolHead));
        auto rl = semaAll4500(prog4500(decl + " var y := b * n; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(rl, kBoolHead));
    }
}

// ===========================================================================
// T4 — ACCEPT-CONTROLS. Each RUNS and asserts a value: a false reject here
// breaks working code, which is worse than the bug the rejection fixes.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t4_int_cast_remedy_still_computes_spec_5_2) {
    // The remedy the diagnostic names must actually work.
    auto r = runAotChecked(prog4500(
        " var i: int := 2; var b: bool := true; return i * int(b) + 40; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t4_decimal_cast_remedy_still_computes_spec_5_2) {
    auto r = runAotChecked(prog4500(
        " var d: decimal := 2m; var b: bool := true; "
        " Console.WriteLine((d * decimal(int(b))).ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2") != std::string::npos);
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t4_ordinary_mixed_numeric_arithmetic_unaffected_spec_6_2) {
    // §6.2 promotion across numeric types is untouched by the widening.
    auto r = runAotChecked(prog4500(
        " var i: int := 2; var l: long := 3; var d: double := 4.0; "
        " var f: float := 5.0f; var m: decimal := 6m; "
        " var a := i * l; var b := i * d; var c := f * d; var e := i * m; "
        " Console.WriteLine($\"{a} {b} {c} {e}\"); return int(a) + 36; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t4_bool_equality_and_logic_still_admitted_spec_3_5) {
    // §3.5: "**Equality** on every primitive (including `bool` …)" and
    // "**Logical** on `bool`: `and`, `or`, `not`". The widening must not
    // over-fire onto either.
    auto r = runAotChecked(prog4500(
        " var a: bool := true; var b: bool := false; "
        " if a = b then return 1; end; "
        " if not (a <> b) then return 2; end; "
        " if a and b then return 3; end; "
        " if not (a or b) then return 4; end; "
        " return 42; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T5 — the ORDERING arm's wording is unchanged. Several lit rows pin its text
// (`sema_bool_ordering_1574__*`), and the widening shares the `report` call
// with it, so a drift there would be invisible to this file without this row.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t5_ordering_diagnostic_unchanged_spec_3_5) {
    auto r = semaAll4500(prog4500(
        " var a: bool := true; var b: bool := false; "
        " if a < b then return 1; end; return 0; "));
    BEZEL_EXPECT_TRUE(hasDiag4500(r, kBoolHead));
    BEZEL_EXPECT_TRUE(hasDiag4500(r, kOrderReason));
    // …and it is NOT given the arithmetic reason clause.
    BEZEL_EXPECT_FALSE(hasDiag4500(r, kArithReason));
}

// ===========================================================================
// T6 — `string + bool` keeps §4.6's concatenation diagnostic. §4.6 is the more
// specific authority for a string operand (`+` there is the declared
// `String.operator +`, §3.5), and its remedy — interpolation or `.ToString()`
// — is the actionable one; `int(b)` is not.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t6_string_plus_bool_keeps_concat_diagnostic_spec_4_6) {
    for (const char* body :
         {" var s: string := \"x\"; var b: bool := true; var t := s + b; "
          " return t.Length; ",
          " var s: string := \"x\"; var b: bool := true; var t := b + s; "
          " return t.Length; "}) {
        auto r = semaAll4500(prog4500(body));
        BEZEL_EXPECT_TRUE(
            hasDiag4500(r, "requires both operands to be `string`"));
        BEZEL_EXPECT_FALSE(hasDiag4500(r, kArithReason));
    }
    // …and the correct spelling still compiles and runs.
    auto ok = runAotChecked(prog4500(
        " var s: string := \"x\"; var b: bool := true; "
        " var t := s + b.ToString(); return t.Length; "));
    BEZEL_EXPECT_TRUE(ok.ran);
    BEZEL_EXPECT_EQ(ok.exitCode, 5);   // "xTrue"
}

// ===========================================================================
// T7 — the bitwise (§6.5) and `mod` (§6.1) `bool` rejections are DIFFERENT
// spec rules with their own remedies, already correct and pinned elsewhere.
// This fix deliberately did not fold them in; these rows pin that decision so
// a later "tidy-up" that collapses them is a visible change, not a silent one.
// ===========================================================================
BEZEL_TEST(sema_bool_operand_arithmetic_4500,
           t7_bitwise_and_mod_keep_their_own_bool_wording) {
    for (std::string_view op : {"&", "|", "^", "<<", ">>"}) {
        auto r = semaAll4500(prog4500(
            std::string(" var b: bool := true; var x := b ") +
            std::string(op) + " 1; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag4500(
            r, "bitwise and shift operators require integer operands"));
        BEZEL_EXPECT_FALSE(hasDiag4500(r, kArithReason));
    }
    auto rm = semaAll4500(
        prog4500(" var b: bool := true; var x := b mod 3; return 0; "));
    BEZEL_EXPECT_TRUE(hasDiag4500(
        rm, "`mod` is defined on the integer types and `decimal`"));
    BEZEL_EXPECT_FALSE(hasDiag4500(rm, kArithReason));
}
