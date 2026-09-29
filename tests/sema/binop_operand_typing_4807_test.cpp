// fix #4807 — the BINARY-operator typer has two gaps that the
// compound-assignment typer does not have (#2163 closed them for compound
// targets): `decimal & int` is silently admitted and lowering degrades to a
// zero-initialized value (#2434 backstop, exit 0, no diagnostic), and
// `string - string` reaches codegen and dies in LLVM IR verification
// ("Integer arithmetic operators only work with integral types! sub ptr").
// Fix #4711 desugars `o op= e` to `o := o op e` for a narrowed `Object`,
// so these gaps are now reachable through the compound spelling too:
// `o &= 1` over `Object` carrying a `decimal` silently emits "store dropped"
// (#2961) and `o -= "b"` over a string ICEs the same way the binary form
// does. Both are addressed in this fix because both reach the binary
// operator typer after #4711's desugaring.
//
// THE RULE, established from the documents before the check was written:
//
//   * §3.5 enumerates each intrinsic operator group BY OPERAND TYPE:
//     "**Bitwise** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`,
//     `long`, `ulong`: `&`, `|`, `^`, `~` (unary). Not valid on `bool`,
//     `char`, or floating-point types (§6.5)." `decimal` is NOT in the
//     enumeration of bitwise operand types — it is grouped with the
//     floating-point kinds for the same rule, mirroring the §6.6 compound
//     typer's existing convention (#2153, #2163, ExprTypecheck.cpp
//     ~18560). §6.5 closes the door: "They are not valid on `bool`, `char`,
//     or floating-point types."
//   * §3.5's "**Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`,
//     `uint`, `long`, `ulong`, `float`, `double`, `decimal`: `+`, `-`, `*`,
//     `/`, unary `-`, and unary `+` (unary `+` is the identity, yielding
//     its operand's value at its operand's type, defined exactly where a
//     numeric operand is; invalid on `bool`, `char`, and `string`)."
//     `string` is absent from the enumeration — the ONLY string operator
//     is `+` (concatenation, §4.6).
//   * §6.6: "In every other case the compound form is a compile error" —
//     and the compound typer's right-hand-operand check is exactly the
//     same §6.5/§3.5 question, so the binary form inherits the gap the
//     compound form already closed (RHS) on the LEFT operand as well.
//
// WHAT WENT WRONG. Measured on 466a28e2 (docs v0.307) before this fix, one
// compile each:
//   `decimal & int` -> compiled clean; lowering degraded through the
//                       #2434 backstop and emitted
//                       "warning: internal: lowerLocalDecl's initializer
//                       expression failed to lower to a value for this
//                       local's slot (degraded to a zero-initialized
//                       value) — #2434". The program ran and printed `0`,
//                       not the bitwise AND of `2` and `1`.
//   `decimal |= int` -> after #4711's desugaring to `o := o | 1`, the same
//                       lowering path: silent wrong code, exit 0, the
//                       only diagnostic is "warning: internal: assignment
//                       RHS expression failed to lower … (store dropped —
//                       target retains its previous value) — #2961".
//   `string - string` -> reached codegen; "Integer arithmetic operators
//                       only work with integral types! sub ptr" ->
//                       "codegen error: IR verification failed
//                       (malformed IR emitted; see report above)". No
//                       sema diagnostic.
//   `string -= string` -> after #4711's desugaring to `o := o - "b"`,
//                        the same ICE; #4711 made this reachable through
//                        the compound spelling for the first time.
//
// THE FIX IS A PAIR OF PRE-EXISTING BACKSTOPS, RE-MATCHED. The §6.6
// compound typer already rejected decimal/string on the RHS (the wrong
// side: the original `decimal & int` shape had `decimal` on the LEFT and
// `int` on the RIGHT, and the compound's RHS check was the only half that
// fired — never on a swapped order). The binary typer's bitwise backstop
// (ExprTypecheck_Exprs.cpp ~7101, the §6.5 gate that already rejects bool,
// char, float, double on EITHER operand) is widened to include `decimal`
// alongside `float`/`double` (the compound typer's same-wording
// convention). The binary typer's arithmetic path has no equivalent
// `string`-operand check at all (the `+` arm above has its §4.6 concat
// rule that admits the both-string form; `-`/`*`/`/`/`mod` have nothing).
// One new `string`-operand rejection is added for `-`/`*`/`/`/`mod`,
// before the §4.6 concat check.
//
// ⚠ This is a REJECTION, so the false-reject hazard dominates; T3/T4/T5/T6
// below are the accept-and-still-emits-correct-value controls.
//
// TEST MATRIX:
//   T1  the issue's two measured rows, through the real driver (`runAot`):
//       rejected with the §6.5 / §4.6 wording, and demonstrably NOT by an
//       IR-verifier failure, crash, signal or assertion — the pre-fix
//       symptom for `string - string`. (`failedAtCompileTimeWithRealLocation`
//       is deliberately NOT used: it passes on a crash's own backstop
//       diagnostic, which is exactly what these rows produced.)
//   T2  the compound form after #4711's `o := o op e` desugaring reaches
//       the binary typer — the narrowed `Object` spelling from the issue
//       repros. `decimal &= 1` and `o -= "b"` over a string both reject.
//   T3  ACCEPT-CONTROLS that RUN and assert a value: `int & int`,
//       `decimal + decimal`, `decimal * decimal`, `string + string`,
//       `decimal + int` (§6.2 widening). None of these admit a code
//       change.
//   T4  all four bitwise/shift ops with decimal — the binary-typer
//       rejection is per-operator AND per-operand, so `|`/`^`/`<<`/`>>`
//       reject identically to `&` (they were not previously tested).
//   T5  the `string` rejection covers `-`/`*`/`/`/`mod` symmetrically
//       and at `string op <numeric>` and `<numeric> op string`. `+`
//       with both operands string is the §4.6 concat case — admitted.
//   T6  the pre-existing bitwise backstop's wording for the rejected
//       kinds (bool, char, float, double) is unchanged: a `bool & int`
//       pair still says "is not valid on `bool` (boolean operations use
//       `and`/`or`/`not`)", a `float & int` still says "is not valid on
//       floating-point types".

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

namespace {

// Parse + collect + resolve + typecheck. Sema only — nothing is compiled or
// run, so an assertion on the diagnostic text here can never be satisfied by
// a crash. Used for the breadth matrices (T4/T5).
bezel::test::SemaResult semaAll4807(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag4807(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr std::string_view kBitwiseHead =
    "is not valid on floating-point types";
constexpr std::string_view kBitwiseSec65 =
    "bitwise and shift operators require integer operands (\xC2\xA7" "6.5)";
constexpr std::string_view kStringArithHead =
    "is not defined on a `string` operand";
constexpr std::string_view kStringArithSec46 =
    "only `+` (concatenation) is defined on `string` (\xC2\xA7" "4.6)";
constexpr std::string_view kStringArithSec35 =
    "(\xC2\xA7" "4.6/\xC2\xA7" "3.5)";

std::string prog4807(std::string_view body) {
    return std::string("module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

// A driver-level (AOT) rejection row: the whole `bezelc` pipeline must refuse
// the program, naming THIS diagnostic, and must never reach codegen — the
// pre-fix symptom for `string - string` was an IR-verifier failure, so
// "rejected correctly" is distinguished from "died" explicitly. The same
// scaffold distinguishes the `decimal & int` case from the #2434 lower-to-
// zero backstop that degraded it silently — the issue's silent-wrong-code
// class, where the only diagnostic was an "internal:" stderr line and the
// program ran with a zero value.
void expectDriverRejectsBitwiseDecimal(const std::string& body) {
    auto r = runAot(prog4807(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kBitwiseHead).c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kBitwiseSec65).c_str()));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout. The
    // #2434 backstop's stderr line is the symptom we are explicitly
    // rejecting: it is NOT a real diagnostic, it is the "internal:"
    // warning that follows the failure to lower. Asserting its absence
    // is what makes this a contract on the SYMBOLIC outcome, not on
    // "something went wrong somewhere".
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "degraded to a zero-initialized value"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "store dropped"));
}

void expectDriverRejectsStringArith(const std::string& body, const char* op) {
    auto r = runAot(prog4807(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("operator `") + op +
            "` " + std::string(kStringArithHead))
               .c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kStringArithSec46).c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kStringArithSec35).c_str()));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's two measured rows, through the real driver.
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t1_decimal_and_int_rejected_spec_6_5) {
    // Pre-fix: compiled clean with the #2434 lower-to-zero backstop, and the
    // program printed `0`. The "internal:" warning is NOT a real diagnostic
    // — the whole point of this rule is that the user is told their
    // program is wrong, not that codegen failed and we degraded to 0.
    expectDriverRejectsBitwiseDecimal(
        " var d: decimal := 2.0m; "
        " Console.WriteLine((d & 1).ToString()); return 0; ");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t1_string_minus_string_rejected_spec_4_6) {
    // Pre-fix: reached codegen; "Integer arithmetic operators only work with
    // integral types! sub ptr" -> "IR verification failed". A sema reject
    // is the entire point of the fix.
    expectDriverRejectsStringArith(
        " var s: string := \"a\"; var t: string := s - \"b\"; "
        " Console.WriteLine(t); return 0; ",
        "-");
}

// ===========================================================================
// T2 — the compound form after #4711's `o := o op e` desugaring reaches the
// binary typer. The narrowed-Object spelling from the issue repros: `o &=
// 1` over `Object` carrying a decimal, and `o -= "b"` over a string. The
// compound form was the only path #4711 made newly reachable; before #4711
// it would have been a clean ICE / silent-zero as the issue notes, but the
// desugaring fixes it now.
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t2_decimal_and_eq_int_rejected_spec_6_5) {
    // Pre-fix: `o &= 1` desugars to `o := o & 1`; the lower-to-zero #2961
    // backstop fired and emitted "store dropped" — the same silent-wrong-
    // code class as T1, reached through the compound spelling.
    expectDriverRejectsBitwiseDecimal(
        " var v0: decimal := 2.0m; var o: Object := v0; "
        " if o is decimal then o &= 1; end; "
        " Console.WriteLine(o.ToString()); return 0; ");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t2_string_minus_eq_string_rejected_spec_4_6) {
    // Pre-fix: `o -= "b"` desugars to `o := o - "b"`, the same codegen
    // crash as T1, now reachable through the compound spelling.
    expectDriverRejectsStringArith(
        " var v0: string := \"a\"; var o: Object := v0; "
        " if o is string then o -= \"b\"; end; "
        " Console.WriteLine(o.ToString()); return 0; ",
        "-");
}

// ===========================================================================
// T3 — ACCEPT-CONTROLS that RUN and assert a value. Every expression the
// rule must leave untouched: §3.5's integer bitwise pair, §6.5's bitwise on
// the integer types, §6.2's mixed-numeric widening (`decimal + int`), §3.5's
// decimal arithmetic, and §4.6's string concatenation. Each row asserts the
// program's stdout matches the expected value, so the accept is a contract on
// the program's behaviour, not on "the compiler did not complain".
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t3_int_and_int_runs_spec_6_5) {
    // Pre-fix behaviour unchanged: `int & int` is §3.5's integer bitwise.
    auto r = runAot(prog4807(
        " var a: int := 5; var b: int := 3; "
        " Console.WriteLine((a & b).ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t3_decimal_plus_decimal_runs_spec_3_5) {
    // Pre-fix behaviour unchanged: `decimal + decimal` is §3.5's arithmetic
    // on the decimal type, plain numeric.
    auto r = runAot(prog4807(
        " var a: decimal := 1.5m; var b: decimal := 2.0m; "
        " Console.WriteLine((a + b).ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.5\n");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t3_decimal_times_decimal_runs_spec_3_5) {
    auto r = runAot(prog4807(
        " var a: decimal := 2.5m; var b: decimal := 4.0m; "
        " Console.WriteLine((a * b).ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t3_string_plus_string_runs_spec_4_6) {
    // Pre-fix behaviour unchanged: `string + string` is §4.6 concatenation.
    auto r = runAot(prog4807(
        " var s: string := \"hello\"; "
        " Console.WriteLine(s + \" world\"); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "hello world\n");
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t3_decimal_plus_int_runs_spec_6_2) {
    // Pre-fix behaviour unchanged: §6.2 widening `int -> decimal`. The
    // control proves the decimal reject does not over-fire on `decimal +
    // <int>` — only the bitwise family rejects decimal, not arithmetic.
    auto r = runAot(prog4807(
        " var a: decimal := 2.5m; var b: int := 4; "
        " Console.WriteLine((a + b).ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6.5\n");
}

// ===========================================================================
// T4 — all four bitwise/shift ops with decimal, in BOTH operand orders.
// §6.5 enumerates "& | ^ << >>" — the rule is per-operator AND per-operand,
// so `|`/`^`/`<<`/`>>` reject identically to `&`. Measured on the binary
// typer's existing pre-fix code path: every one was silently admitted; the
// §6.6 compound typer's RHS-only check left the LEFT side open in
// compound form too.
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t4_all_bitwise_ops_with_decimal_rejected_spec_6_5) {
    for (std::string_view op : {"&", "|", "^"}) {
        auto r = semaAll4807(prog4807(
            std::string(" var d: decimal := 2.0m; return d ") +
            std::string(op) + " 1; "));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseHead));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseSec65));
    }
    for (std::string_view op : {"&", "|", "^"}) {
        auto r = semaAll4807(prog4807(
            std::string(" var d: decimal := 2.0m; return 1 ") +
            std::string(op) + " d; "));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseHead));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseSec65));
    }
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t4_shift_ops_with_decimal_rejected_spec_6_5) {
    // Shifts too — §6.5 enumerates << / >> on the integer types, so decimal
    // is out. The `count` operand is the same decimal, exercising the rule
    // symmetrically.
    for (std::string_view op : {"<<", ">>"}) {
        auto r = semaAll4807(prog4807(
            std::string(" var d: decimal := 2.0m; return d ") +
            std::string(op) + " 1; "));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseHead));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseSec65));
    }
}

// ===========================================================================
// T5 — the `string` reject is symmetric across `-`/`*`/`/`/`mod` and across
// operand orders. The rule reads per operand: `s op <T>` and `<T> op s`
// both reject. `s + s` is the §4.6 concat case (admitted, T3 above); `s +
// 5` is the §4.6 mixed-string case (its own wording, not this one).
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t5_all_arith_ops_with_string_rejected_spec_4_6) {
    for (std::string_view op : {"-", "*", "/", "mod"}) {
        auto r = semaAll4807(prog4807(
            std::string(" var s: string := \"a\"; return s ") +
            std::string(op) + " \"b\"; "));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kStringArithHead));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kStringArithSec46));
    }
    for (std::string_view op : {"-", "*", "/", "mod"}) {
        auto r = semaAll4807(prog4807(
            std::string(" var s: string := \"a\"; return 1 ") +
            std::string(op) + " s; "));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kStringArithHead));
        BEZEL_EXPECT_TRUE(hasDiag4807(r, kStringArithSec46));
    }
}

// ===========================================================================
// T6 — the pre-existing bitwise backstop's wording for the rejected kinds
// (bool, char, float, double) is UNCHANGED by the widening: a `bool & int`
// pair still says "is not valid on `bool` (boolean operations use `and`/
// `or`/`not`)", a `float & int` still says "is not valid on floating-point
// types". The widening only adds `decimal` to the "floating-point types"
// bucket. lit rows pin both wordings.
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t6_pre_existing_bool_wording_unchanged_spec_6_5) {
    auto r = semaAll4807(prog4807(
        " var b: bool := true; return b & 1; "));
    BEZEL_EXPECT_TRUE(hasDiag4807(r,
        "is not valid on `bool` (boolean operations use `and`/`or`/`not`)"));
    // NOT the new wording — `bool` is still its own bucket, not absorbed
    // into "floating-point types".
    BEZEL_EXPECT_FALSE(hasDiag4807(r, kBitwiseHead));
}

BEZEL_TEST(sema_binop_operand_typing_4807,
           t6_pre_existing_float_wording_unchanged_spec_6_5) {
    auto r = semaAll4807(prog4807(
        " var f: float := 1.0; return f & 1; "));
    // `float` still says "floating-point types" — the new wording is
    // *shared* with `decimal`, not new for `float`.
    BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseHead));
    BEZEL_EXPECT_TRUE(hasDiag4807(r, kBitwiseSec65));
}
// ===========================================================================
// T7 — §25.2 (v0.307): "a binary overload may pair the declaring type with
// any other type in either position". A user operator resolved for a
// `V op string` / `string op V` pair is a call, not built-in string
// arithmetic, so the §4.6 string-operand rule must not reach it. Regression
// for the fix-forward on f0d699db, which ran the rule after EVERY path of
// typeBinary (including the §25 user-operator return) and rejected all three
// operators below — plus retyped them `string`, cascading into "string member
// `.X`" errors. Accepted at 78470f01; RUN and assert the computed values.
// ===========================================================================
BEZEL_TEST(sema_binop_operand_typing_4807,
           t7_user_operator_with_string_operand_accepted_spec_25_2) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, s: string): V => "
        "new V(v.X + s.Length); "
        "  public static method operator -(s: string, v: V): V => "
        "new V(s.Length - v.X); "
        "  public static method operator mod(v: V, s: string): int => v.X; "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(1); var w := v * \"abc\"; "
        "    var z := \"abcd\" - v; var m := v mod \"q\"; "
        "    Console.WriteLine(w.X); Console.WriteLine(z.X); "
        "    Console.WriteLine(m); return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n3\n1\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "is not defined on a `string` operand"));
}
