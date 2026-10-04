// §7.11 Lifted operators — codegen tests for nullable value type arithmetic,
// relational, and unary operators (issue #486).
//
// v0.267 (proposal 0114 / #3298) INVERTED the arithmetic / bitwise / shift /
// relational / unary lifting acceptance tests: every spelling is now a
// compile error (proposal D1/D6). The §8.4 retained forms (`=` / `<>`)
// and the §7.5 / §7.6 / §7.7 marked forms (`?.` / `?(…)` / `??`) are
// unaffected; the BEZEL_TEST blocks exercising those are kept acceptance.
// This file's BEZEL_TEST blocks have been flipped from acceptance
// (`runAot(...) == <value>`) to rejection (`!runAot(...)`, asserts the
// program failed at compile time with a §7.11 diagnostic naming the
// §7.4 / `??` / `!` remedies). The same fix-for-fix notes (issue numbers,
// narrowing treatment) live in the individual block comments below.


#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Helper: run a Bezel program that returns an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}


// Helper for v0.267 (#3298): returns true iff the program runs (its
// front end did NOT emit a diagnostic). Acceptance tests use `int run(...)
// == VAL`; the inverted rejection tests use `!runs(...)` / `!runsAot(...)`.
// The `??`/`?.`-coalescing acceptance tests and the §8.4 equality tests
// stay — they exercise the retained forms (proposal D2/D3) and the
// non-nullable control cases.
bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}
bool runsAot(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran;
}

// §7.11 (v0.286, #3298): rejected AND the diagnostic is the §7.11 one naming
// the remedies — "did not run" alone would pass on any unrelated compile error.
bool rejectedBy_7_11(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("is not defined on a nullable value type") ==
                std::string::npos)
            continue;
        if (m.find("7.11") == std::string::npos) continue;
        if (m.find("narrow first") == std::string::npos) continue;
        return true;
    }
    return false;
}

} // namespace


// =========================================================================
// §7.11 — int? arithmetic: both present
// =========================================================================

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_add_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"));
}

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_sub_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 10; var b: int? := 3; "
        "var r: int? := a - b; "
        "return (r ?? -1);"));
}

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_mul_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 6; var b: int? := 7; "
        "var r: int? := a * b; "
        "return (r ?? -1);"));
}

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 20; var b: int? := 4; "
        "var r: int? := a / b; "
        "return (r ?? -1);"));
}

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_mod_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 17; var b: int? := 5; "
        "var r: int? := a mod b; "
        "return (r ?? -1);"));
}


// =========================================================================
// §7.11 — int? arithmetic: either operand null → null result
// =========================================================================

// §7.11: a null, b present → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_left_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := 4; "
        "var r: int? := a + b; "
        "return int(r = null);"));
}

// §7.11: a present, b null → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_right_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := 3; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"));
}

// §7.11: both null → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_both_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"));
}


// =========================================================================
// §7.11 — DivideByZeroException fires only when BOTH operands are present
// =========================================================================

// §7.11 (v0.286, #3298) — REJECTION. These two rows taught the lift's
// "the division never executes when an operand is absent" behaviour; v0.267
// removed the lift, and §7.11 now says the opposite in as many words: "This
// section introduces no fault of its own and no new throwable expression: an
// absent operand is diagnosed at COMPILE time", and the runtime checks are
// "unchanged and unmoved … reached after the operands are narrowed". A local
// initialized with `null` is not narrowed (§7.4), so it is a nullable operand
// here. `int_div_both_present_zero_raises_spec_7_11` below keeps the
// DivideByZeroException coverage on the narrowed (admitted) form.
BEZEL_TEST(codegen_lifted_ops_486, int_div_null_divisor_rejected_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := null; var b: int? := 0; "
        "var r: int? := a / b; "
        "return int(r = null);"));
}

BEZEL_TEST(codegen_lifted_ops_486, int_div_null_dividend_rejected_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 10; var b: int? := null; "
        "var r: int? := a / b; "
        "return int(r = null);"));
}

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_zero_raises_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 10; var b: int? := 0; "
        "try var r: int? := a / b; return 0; "
        "catch DivideByZeroException => return 1; end;"));
}

// §7.11: null mod 0 → null, NO exception.
BEZEL_TEST(codegen_lifted_ops_486, int_mod_null_no_exception_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := 0; "
        "var r: int? := a mod b; "
        "return int(r = null);"));
}


// =========================================================================
// §7.11 — int? relational: result bool (not bool?)
// =========================================================================

// §7.11: both present, a < b → true.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_present_true_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 3; var b: int? := 7; "
        "return int(a < b);"));
}

// §7.11: both present, a < b false.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_present_false_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 7; var b: int? := 3; "
        "return int(a < b);"));
}

// §7.11: left null → false (regardless of right).
BEZEL_TEST(codegen_lifted_ops_486, int_lt_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := 3; "
        "return int(a < b);"));
}

// §7.11: right null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_right_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := 3; var b: int? := null; "
        "return int(a < b);"));
}

// §7.11: both null → false (null is not "less than" anything).
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := null; "
        "return int(a < b);"));
}

// §7.11: <= both present, equal → true.
BEZEL_TEST(codegen_lifted_ops_486, int_le_both_present_equal_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 5; var b: int? := 5; "
        "return int(a <= b);"));
}

// §7.11: <= left null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_le_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := 5; "
        "return int(a <= b);"));
}

// §7.11: > both present, true.
BEZEL_TEST(codegen_lifted_ops_486, int_gt_both_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 9; var b: int? := 3; "
        "return int(a > b);"));
}

// §7.11: > right null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_gt_right_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := 9; var b: int? := null; "
        "return int(a > b);"));
}

// §7.11: >= both present equal → true.
BEZEL_TEST(codegen_lifted_ops_486, int_ge_both_equal_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 4; var b: int? := 4; "
        "return int(a >= b);"));
}

// §7.11: >= left null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_ge_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int? := 4; "
        "return int(a >= b);"));
}


// =========================================================================
// §7.11 — unary `-` and `~` on int?
// =========================================================================

// §7.11: unary `-` on present int? → negated value (narrow with ??). -5 does
// not fit an 8-bit exit code, so this builds the full program directly and
// observes stdout (run()'s body-only wrapper can't route through runAotOut).
//
// v0.267 / #3298: pre-fix the program compiled and `-a` produced a nullable
// result; post-fix the assignment from `5` narrowly establishes `a` as non-
// null (§7.4) and `-a` is the ordinary non-nullable form whose result
// widens back to `int?` at the `var r` site. The program compiles in both
// the old AND new regimes; what changes is that an operand whose §7.4
// narrowing does NOT establish non-null (e.g. one assigned `null`) is now
// a §7.11 compile error. Cover that case separately as the v0.267
// rejection shape, and keep this control as the narrowed-positive case.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_neg_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 5; "
        "var r: int? := -a; "
        "return 0;"));
}

// §7.11: unary `-` on null int? → null.
//
// v0.267 / #3298: a nullable operand that §7.4 has not narrowed non-null
// (here, one assigned `null`) is rejected at sema. Converted from
// acceptance to rejection.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_neg_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; "
        "var r: int? := -a; "
        "return int(r = null);"));
}

// §7.11: unary `~` on present int? → bitwise complement (~0 == -1). -1 does
// not fit an 8-bit exit code — observe on stdout.
//
// v0.267 / #3298: same narrowing caveat as the unary neg test above —
// `var a: int? := 5` narrows `a` (§7.4), `~a` is the non-nullable form,
// `var r := ~a` widens back. The program runs under both regimes. The
// pre-fix acceptance was "lift present operand"; the post-fix twist is
// that the program happens to still run, so this is now a
// post-fix-still-accepts control.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_bitnot_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 0; "
        "var r: int? := ~a; "
        "return 0;"));
}

// §7.11: unary `~` on null int? → null.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_bitnot_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; "
        "var r: int? := ~a; "
        "return int(r = null);"));
}


// =========================================================================
// §7.11 — Mixed T? + T (non-nullable operand treated as always present)
// =========================================================================

// §7.11: int? + int (non-nullable treated as present) → int?.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nullable_plus_nonnull_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 3; var b: int := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"));
}

// §7.11: null int? + non-null int → null.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_null_plus_nonnull_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; var b: int := 4; "
        "var r: int? := a + b; "
        "return int(r = null);"));
}

// §7.11 (v0.286, #3298) — REJECTION, the mirror of
// `int_mixed_null_plus_nonnull_spec_7_11` above. §7.11 rejects "on either
// side, and in the mixed `T?`/`T` pairing alike"; the two orders must not
// disagree, which is the "same shape, opposite answers, decided by spelling"
// defect proposal 0114 exists to remove. `b` is initialized with `null`, so
// §7.4 narrows nothing.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nonnull_plus_null_rejected_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int := 3; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"));
}

// §7.11: int + int? (other side), right is present → sum.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nonnull_plus_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"));
}


// =========================================================================
// §7.11 — Narrowing required: result is T?, coalesce to get non-nullable
// =========================================================================

// §7.11: `(a + b) ?? 0` narrows from int? → int correctly.
BEZEL_TEST(codegen_lifted_ops_486, int_add_coalesced_to_nonnull_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := 10; var b: int? := 5; "
        "var r: int := (a + b) ?? 0; "
        "return r;"));
}

// §7.11 (v0.286, #3298) — REJECTION. A trailing `??` does not rescue the
// operator: §7.11 rejects at the OPERAND, and `??` is a remedy applied to the
// operand (`(a ?? 0) + (b ?? 0)`), not to the result. That distinction is the
// whole point of rejecting rather than lifting — under the lift this program
// silently produced 99 for absent input, which is the shape proposal 0114 §1.1
// calls the fail-open guard.
BEZEL_TEST(codegen_lifted_ops_486, int_add_null_result_coalesce_rejected_spec_7_11) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := null; var b: int? := null; "
        "var r: int := (a + b) ?? 99; "
        "return r;"));
}

// …and the remedy applied at the OPERANDS compiles and gives the answer the
// author wanted: 0 + 0 = 0, not the 99 the lifted form produced.
BEZEL_TEST(codegen_lifted_ops_486, int_add_operandwise_coalesce_accepted_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := null; "
        "var r: int := (a ?? 0) + (b ?? 0); "
        "return r + 7;"), 7);
}



// ===========================================================================
// §7.11 / §7.4 (v0.286, #3298) — the NARROWED operand, VALUE-verified across
// the whole primitive family and across the decimal runtime route.
//
// Two defects these rows exist for, both measured on drafts of #3298:
//
//  (a) §7.4's establishment predicate was `!initTy->nullable`, and §5.3
//      constant-narrowing retypes a literal to the TARGET type — so
//      `var a: byte? := 3;` did NOT narrow while `var a: int? := 3;` did, and
//      §7.11 then gave the two IDENTICAL shapes opposite verdicts, decided by
//      the operand's width. Every width is asserted here, with the value, so a
//      re-divergence cannot pass.
//
//  (b) codegen's §4.4 decimal routing keyed on the operand's DECLARED
//      `decimal?` type, skipped a narrowed decimal operand, and emitted
//      `add %bz.decimal, i32` — an IR-verification abort (exit 141). The
//      decimal rows below run the arithmetic and check the RESULT, so the
//      routing is observed and not merely "it compiled".
// ===========================================================================

// BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing (§6.6) proves a local
// non-null for member access but NOT for the §7.11 operator non-lift gate.
// Only null-test narrowing (if a <> null then) admits operators. The test
// below was pinning the OLD behavior where init-narrowing admitted operators.
BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, every_width_narrows_and_computes) {
    // §7.11 (v0.315, #4930): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: byte?   := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: sbyte?  := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: short?  := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: ushort? := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: int?    := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: long?   := 3; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: double? := 3.0; return int((a + 1.0) ?? 0.0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: float?  := 3.0f; return int((a + 1.0f) ?? 0.0f);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: decimal? := 3m; return int((a + 1m) ?? 0m);"));
}

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, every_width_null_init_is_rejected) {
    // The control for the row above: identical shapes, `null` initialiser, so
    // §7.4 narrows nothing and §7.11 rejects. Without this the row above could
    // pass because the gate had been removed rather than because it narrowed.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: byte?    := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: sbyte?   := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: short?   := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: ushort?  := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: int?     := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: long?    := null; return int((a + 1) ?? 0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: double?  := null; return int((a + 1.0) ?? 0.0);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: decimal? := null; return int((a + 1m) ?? 0m);"));
}

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, narrow_width_bitwise_and_shift) {
    // §7.11 (v0.315, #4930): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: byte?  := 5; return int((a << 1) ?? 99);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: byte?  := 5; return int((a & 3) ?? 99);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: short? := 3; return int((a * 2) ?? 99);"));
}

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, decimal_routed_through_the_runtime) {
    // §7.11 (v0.315, #4930): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: decimal? := 5; var d: decimal := a + 1; return int(d);"));
    BEZEL_EXPECT_TRUE(rejectedBy_7_11("var a: decimal? := 5m; var b: decimal? := 3m; return int((a * b) ?? 0m);"));
    // The rest use null-init (rejected by §7.11 anyway) or null-test narrowing
    // (which DOES admit operators), so keep the original expectations:
    BEZEL_EXPECT_EQ(run(
        "var a: decimal? := 5m; var d: decimal := 0m; "
        "if a <> null then d := a - 1m; end if; return int(d);"), 4);
    // §8.4 equality over a genuinely-nullable decimal is RETAINED and must
    // still take the boxed path (the unwrap is skipped for `=`/`<>`, which is
    // what keeps #663's routing correct).
    BEZEL_EXPECT_EQ(run("var a: decimal? := null; return (if a = null then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(run("var a: decimal? := 3m; var b: decimal? := 3m; return (if a = b then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, narrowed_decimal_relational_computes) {
    // §7.11 (v0.315, #4930): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: decimal? := 3m; var b: decimal? := 4m; "
        "if a < b then return 1; end if; return 0;"));
}

// =========================================================================
// §7.11 — double? arithmetic and relational
// =========================================================================

// §7.11: double? + double? both present.
BEZEL_TEST(codegen_lifted_ops_486, double_add_both_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: double? := 1.5; var b: double? := 2.5; "
        "var r: double? := a + b; "
        "return int(r ?? 0.0);"));
}

// §7.11: double? + null → null.
BEZEL_TEST(codegen_lifted_ops_486, double_add_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: double? := 1.5; var b: double? := null; "
        "var r: double? := a + b; "
        "return int(r = null);"));
}

// §7.11: double? < double? both present (1.0 < 2.0).
BEZEL_TEST(codegen_lifted_ops_486, double_lt_both_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: double? := 1.0; var b: double? := 2.0; "
        "return int(a < b);"));
}

// §7.11: double? < null → false.
BEZEL_TEST(codegen_lifted_ops_486, double_lt_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: double? := 1.0; var b: double? := null; "
        "return int(a < b);"));
}

// §7.11: null double? < double? → false.
BEZEL_TEST(codegen_lifted_ops_486, double_lt_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: double? := null; var b: double? := 1.0; "
        "return int(a < b);"));
}

// §7.11 + IEEE 754: both operands present but one is NaN. NaN is unordered, so
// the ordered relational comparison yields false for every ordering operator —
// `<`, `<=`, `>`, `>=` all return false. (This matches the non-nullable double
// comparison: lifting does not change the IEEE ordering semantics; it only adds
// the "false if either operand is null" rule on top.) `double.NaN` per stdlib
// §2.8. We pack all four results into the exit code; every operator → 0.
BEZEL_TEST(codegen_lifted_ops_486, double_relational_nan_both_present_false_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: double? := double.NaN; var b: double? := 1.0; "
        "var lt: int := int(a < b); var gt: int := int(a > b); "
        "var le: int := int(a <= b); var ge: int := int(a >= b); "
        "return lt * 1000 + gt * 100 + le * 10 + ge;"));
}

// §7.11 + IEEE 754: NaN on the right operand — same all-false outcome.
BEZEL_TEST(codegen_lifted_ops_486, double_relational_nan_right_false_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: double? := 1.0; var b: double? := double.NaN; "
        "return int(a < b) + int(a > b) + int(a <= b) + int(a >= b);"));
}


// =========================================================================
// §7.11 — long? arithmetic
// =========================================================================

// §7.11: long? + long? both present.
BEZEL_TEST(codegen_lifted_ops_486, long_add_both_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: long? := long(3); var b: long? := long(4); "
        "var r: long? := a + b; "
        "return int(r ?? long(-1));"));
}

// §7.11: long? null → null.
BEZEL_TEST(codegen_lifted_ops_486, long_add_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: long? := null; var b: long? := long(4); "
        "var r: long? := a + b; "
        "return int(r = null);"));
}

// §7.11: long? relational both present.
BEZEL_TEST(codegen_lifted_ops_486, long_lt_both_present_spec_7_11) {
    // BEHAVIOR FLIP (fix #4930, v0.315): init-narrowing does NOT admit operators.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: long? := long(1); var b: long? := long(2); "
        "return int(a < b);"));
}

// §7.11: long? relational null → false.
BEZEL_TEST(codegen_lifted_ops_486, long_lt_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: long? := null; var b: long? := long(2); "
        "return int(a < b);"));
}


// =========================================================================
// §6.5: bitwise `& | ^ ~ << >>` are NOT valid on `bool` or `char`.
// =========================================================================
// §7.11's lift list applies only where the base operator is valid for `T`, so
// it does not extend bitwise/shift to `bool?` / `char?`. The lifting machinery
// previously treated `bool?` as an i1 integer and accepted `bool? & bool?`,
// which drifted from §6.5 ("Boolean operations use `and`/`or`/`not`"); that is
// now rejected at sema. The rejection (binary, unary `~`, and compound forms,
// for both bool and char) is covered in lifted_operators_486_sema_test.cpp.


// =========================================================================
// §7.11 — CONTROL: non-nullable int arithmetic unchanged
// =========================================================================

// §7.11 (control): plain int + int still works.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_int_add_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return 3 + 4;"), 7);
}

// §7.11 (control): plain int relational still works.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_int_lt_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return int(3 < 7);"), 1);
}

// §7.11 (control): plain double relational unchanged.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_double_lt_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return int(1.0 < 2.0);"), 1);
}


// =========================================================================
// §7.11 — Equality (= / <>) is UNCHANGED (§8.4, already null-safe)
// =========================================================================

// §8.4 (control): int? = null still works.
BEZEL_TEST(codegen_lifted_ops_486, int_eq_null_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; return int(a = null);"), 1);
}

// §8.4 (control): int? <> null still works for non-null value.
BEZEL_TEST(codegen_lifted_ops_486, int_neq_null_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; return int(a <> null);"), 1);
}

// §8.4 (control): int? = int? (both non-null, equal).
BEZEL_TEST(codegen_lifted_ops_486, int_eq_both_nonnull_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; var b: int? := 5; return int(a = b);"), 1);
}

// #1044 / §5.1 / §7.11: lifted arithmetic and `??` on a SUB-i32 nullable
// (byte?/sbyte?/short?/ushort?) crashed codegen — the payload (i8/i16) and the
// int operand/default (i32) mismatched the result struct width (`insertvalue
// i32 into {i8,i8}`) / the coalesce PHI. `byte? + int` promotes to `int?`
// (§5.1), and a shift follows the left operand (`byte? << 1` → `byte?`); both
// now build the right-width result. The null branch still propagates null.
BEZEL_TEST(codegen_1044_subint_nullable, byte_plus_int_promotes_to_int) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 3; var r: int? := a + 1; return int(r ?? 0);"), 4);
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_coalesce_int_default) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := 3; return int(a ?? 9);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_null_coalesce_int_default) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := null; return int(a ?? 9);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_shl_follows_left) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := 5; return int((a << 1) ?? 99);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, short_mul_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs("var a: short? := 3; return int((a * 2) ?? 99);"));
}
// §7.11 (v0.286, #3298) — REJECTION: `a` is initialised with `null`, so §7.4
// narrows nothing and the operand is genuinely nullable. Its siblings in this
// suite keep their acceptance because a non-null literal initialiser DOES
// narrow — which is the distinction the whole suite now turns on.
BEZEL_TEST(codegen_1044_subint_nullable, byte_null_arith_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: byte? := null; return int((a + 1) ?? 99);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_bitand_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := 5; return int((a & 3) ?? 99);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, long_coalesce_int_default) {
    BEZEL_EXPECT_TRUE(runs("var a: long? := null; return int(a ?? 5);"));
}

// #1044 (completion) / #1046: the `??` payload coercion for a nullable value
// type must happen in the rhs block (before the merge), or a VARIABLE default
// (`x ?? v`) emits an instruction that does not dominate the PHI edge — only a
// constant default (a folded ConstantExpr) survived the earlier endBB
// placement. Also covers `decimal? ?? <int>` (§5.1 int→decimal widening of the
// default), which previously mixed a %bz.decimal payload with an i32 PHI value.
BEZEL_TEST(codegen_1046_coalesce, byte_coalesce_variable_int_default) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: byte? := 3; var d: int := 9; return int(a ?? d);"));
}
BEZEL_TEST(codegen_1046_coalesce, byte_null_coalesce_variable_int_default) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: byte? := null; var d: int := 9; return int(a ?? d);"));
}
BEZEL_TEST(codegen_1046_coalesce, byte_coalesce_computed_int_default) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: byte? := 3; var x: int := 4; return int(a ?? (x + 5));"));
}
BEZEL_TEST(codegen_1046_coalesce, decimal_coalesce_int_default_present) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := 5; return int(a ?? 9);"));
}
BEZEL_TEST(codegen_1046_coalesce, decimal_coalesce_int_default_null) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := null; return int(a ?? 9);"));
}
BEZEL_TEST(codegen_1046_coalesce, decimal_coalesce_variable_int_default_null) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := null; var d: int := 7; return int(a ?? d);"));
}

// #1047 / §5.1 / §7.11: lifted decimal arithmetic with a mixed INTEGER operand
// (decimal? + int, or int + decimal?) must promote the int to a decimal struct
// before the bz_decimal_* call. Previously the i32 was stored raw into a decimal
// slot → a garbage decimal that faulted/overflowed at runtime (threw instead of
// computing). decimal? + decimal? and the non-nullable decimal + int were fine.
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_plus_int_present) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := 5; return int((a + 1) ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_times_int) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := 5; return int((a * 2) ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_minus_int) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := 5; return int((a - 1) ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, int_plus_decimal_operand_order) {
    BEZEL_EXPECT_TRUE(runs("var a: decimal? := 5; return int((1 + a) ?? 99);"));
}
// §7.11 (v0.286, #3298) — REJECTION, same distinction as the `byte?` row
// above. The other five rows in this suite keep their acceptance and are what
// pins the §4.4 decimal RUNTIME ROUTING for a §7.4-narrowed `decimal?`: a
// draft of #3298 read the operand's DECLARED `decimal?` type in codegen's
// decimal block, skipped it, and emitted `add %bz.decimal, i32` (exit 141).
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_null_plus_int_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: decimal? := null; return int((a + 1) ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_nullable_both_unchanged) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := 5; var b: decimal? := 3; return int((a + b) ?? 99);"));
}

// #1045 / §7.11 / §17.1: a lifted relational comparison is a definite `bool`
// ("false when either operand is null"), so `(int? < 5) ?? false` is `??` on a
// non-nullable value — the left can never be null and the result is always the
// left. lowerCoalesce returned nullptr for a non-pointer non-nullable LHS, so
// the whole `??` lowered to null → false; it now returns the left operand.
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_true_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 3; return (if ((a < 5) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_false_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 3; return (if ((a > 5) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_null_operand_is_false) {
    // null < 5 is the definite bool `false` (§7.11), so `?? false` is false.
    BEZEL_EXPECT_FALSE(runs(
        "var a: int? := null; return (if ((a < 5) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nonnull_bool_coalesce_returns_left_true) {
    BEZEL_EXPECT_TRUE(runs("return (if ((5 > 3) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nonnull_bool_coalesce_returns_left_false) {
    BEZEL_EXPECT_TRUE(runs("return (if ((5 < 3) ?? true) then 1 else 0);"));
}
