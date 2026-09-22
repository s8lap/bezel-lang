// fix #4513 — the §6.6 compound-assignment family check carried a THIRD,
// independent answer to "may these two operand types meet at this
// operator?", beside the two `typeBinary` copies fix #4501 already unified
// (§3.5 / §5.1 / §6.2 / §6.6, v0.306).
//
// THE BUG. `int += bool`, `int *= string`, and `decimal *= bool` are each a
// compile error as BINARY expressions: §3.5 admits no `bool` arithmetic
// operand (fix #4500) and §5.1 relates neither pair (fix #4501/#4512) — so
// the binary path rejects them outright, with no result type at all. But the
// §6.6 compound-assignment check (ExprTypecheck.cpp, the "Arithmetic" family
// arm) computed its OWN, independent "R = type of `target op value`" by
// widening `target`/`value` in isolation, with no §3.5/§5.1 gate at all: for
// exactly these three pairs, the "no conversion either way" fallback set
// `rPrim = rTy->primitive` and reported "`int + bool` computes `bool`" /
// "`int * string` computes `string`" / "`decimal * bool` computes `bool`" —
// asserting a binary result type the binary path does not compute. Measured
// on 67b79744 (docs v0.306), one compile each:
//
//   `i += b` (int, bool)      -> "compound assignment `+=` has no valid
//                                 result: `int + bool` computes `bool` …
//                                 (§6.6)"
//   `i *= s` (int, string)    -> "… `int * string` computes `string` … (§6.6)"
//   `d *= b` (decimal, bool)  -> "… `decimal * bool` computes `bool` … (§6.6)"
//
// THE FIX. The compound-assignment "Arithmetic" arm now asks the binary
// path's OWN two admissibility questions — via the SAME calls, not new
// copies — before computing R at all:
//   1. `ExprTC::reportBoolOperandExcludedFromArithmeticOrOrdering` (§3.5),
//      extracted from fix #4500's inline `typeBinary` gate.
//   2. `ExprTC::reportNoImplicitConversionBetweenOperands` (§5.1/§6.2), the
//      SAME call fix #4501 unified `typeBinary`'s two copies onto, now with
//      a SourceLocation/Expr* overload for callers with no `ast::BinaryExpr`
//      (impl.hpp).
// A pair either gate excludes is reported with the SAME wording as the
// binary rejection and the R computation never runs — it no longer names a
// type the binary path does not compute.
//
// NOT changed, deliberately: the ALREADY-correct §6.2-inadmissible
// integer/integer (or char/integer) pair — `int += ulong`, pinned by the
// #2155 lit rows with `reportInadmissiblePair`'s own wording — is scoped OUT
// of the new gates (both operands are integer-kinded, so the pre-existing
// `reportInadmissiblePair` path still owns it, unchanged). See
// ExprTypecheck.cpp's `integerKindPair` comment for why.
//
// One case's WORDING does change as a side effect of the same unification,
// pinned separately by the lit track: `decimal += <double>` (no §5.1
// relation in either direction) now gets the pairwise-mismatch wording
// instead of a write-back-narrows wording that presupposed a computed R this
// pair never has — see
// tests/lit/sema/compound_assign_cross_family_1689__decimal_plus_double_rejected.bzl.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

std::string prog4513(std::string_view body) {
    return std::string(
               "module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

}  // namespace

// §3.5/§6.6 (fix #4513): `int += bool` gets the SAME §3.5 "not defined for
// `bool` operands" wording the binary `int + bool` reports (fix #4500), NOT
// the old "computes `bool`" §6.6 write-back wording — the binary form has no
// result at all, so nothing "computes" anything.
BEZEL_TEST(compound_assign_operand_pair_unification_4513,
           int_plus_equals_bool_rejected_as_bool_operand_spec_3_5) {
    auto r = runAot(prog4513(
        " var i: int := 2; var b: bool := true; i += b; return i; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for `bool` operands"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "3.5"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "computes `bool`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "has no valid result"));
}

// §3.5/§6.6 (fix #4513): `decimal *= bool` — the non-integer-target sibling
// row; `decimal` is not integer-kinded, so the pre-fix code's
// `integerWriteBack` concession could never rescue it either, and it always
// fell straight to the wrong "computes `bool`" wording.
BEZEL_TEST(compound_assign_operand_pair_unification_4513,
           decimal_times_equals_bool_rejected_as_bool_operand_spec_3_5) {
    auto r = runAot(prog4513(
        " var d: decimal := 2m; var b: bool := true; "
        " d *= b; return int(d); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined for `bool` operands"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "3.5"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "computes `bool`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "has no valid result"));
}

// §5.1/§6.2/§6.6 (fix #4513): `int *= string` gets the SAME §5.1 pairwise-
// mismatch wording the binary `int * string` reports (fix #4501), NOT the
// old "computes `string`" §6.6 write-back wording.
BEZEL_TEST(compound_assign_operand_pair_unification_4513,
           int_times_equals_string_rejected_as_pairwise_mismatch_spec_5_1) {
    auto r = runAot(prog4513(
        " var i: int := 2; var s: string := \"x\"; "
        " i *= s; return i; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` has no implicit conversion between operand types "
           "`int` and `string`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7" "5.1"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "computes `string`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "has no valid result"));
}

// Negative control (§2155-class, UNCHANGED): the already-correctly-rejected
// integer/integer §6.2-inadmissible pair keeps `reportInadmissiblePair`'s own
// wording — this fix must not touch it (both operands are integer-kinded, so
// the new gates are scoped out; see ExprTypecheck.cpp's `integerKindPair`).
BEZEL_TEST(compound_assign_operand_pair_unification_4513,
           int_plus_equals_ulong_var_keeps_2155_wording_spec_6_2) {
    auto r = runAot(prog4513(
        " var i: int := -1; var u: ulong := 2UL; i += u; return i; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "compound assignment `+=` has no implicit conversion between "
           "operand types `int` and `ulong`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "has no valid result"));
}

// Accept-controls: the legitimate narrow-integer write-back (§6.6's OWN
// concession) and the §5.1 constant-narrowing escape (fix #4512) both still
// admit through the new gates — neither is a bool/string mismatch, so
// `reportBoolOperandExcludedFromArithmeticOrOrdering` never fires, and both
// operands are integer-kinded in the first row (scoped out of the new §5.1
// gate) / the pair is same-type in the second (the shared gate's own
// same-type fast path).
BEZEL_TEST(compound_assign_operand_pair_unification_4513,
           legitimate_write_back_and_literal_escape_still_admitted) {
    auto r = runAot(prog4513(
        " var b: byte := 10; b += 1; "                 // narrow-int write-back
        " var u: ulong := 5; u += 100; "                // §5.1 literal escape
        " return int(b) + int(u); "));
    BEZEL_EXPECT_TRUE(r.ran);
    // per CLAUDE.md's 8-bit exit-truncation note: kept well under 256 so the
    // observable is not a WEXITSTATUS-masked value.
    BEZEL_EXPECT_EQ(r.exitCode, 11 + 105);
}
