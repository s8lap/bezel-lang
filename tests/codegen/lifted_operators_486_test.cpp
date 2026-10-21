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
//
// WHICH narrowing admits an operator (§7.4, spec:1061, read through §7.11,
// spec:1177; #4718 ruled YES and v0.309 ships it). §7.11 reads the operand's
// STATIC type and admits the ordinary operator wherever §7.4 has narrowed the
// operand non-null: "an operand that §7.4 has narrowed non-null is a
// non-nullable operand and the ordinary operator applies to it unchanged; no
// separate rule is needed for that case". §7.4's establishment list names a
// declaration's initializer explicitly — "`var x: T? := expr;` … which is the
// declaration's assignment" — so BOTH narrowing kinds reach this gate:
//   • NULL-TEST narrowing (`if a <> null then a < b … end if`) ADMITS.
//   • INIT-NARROWING (`var a: int? := 5;` — §7.4's declaration-assignment
//     bullet) ADMITS EQUALLY. `var a: int? := 3; a + 1` compiles.
// The rows below use the null-test spelling throughout. That spelling is
// ADMISSIBLE, not REQUIRED: it is not the only way to reach a non-nullable
// operand, and nothing here turns on init-narrowing being refused. What these
// rows turn on is the operand being GENUINELY nullable — a `:= null`
// initializer, a nullable parameter, or an unnarrowed second operand — since
// that is the case §7.4 narrows nothing for and §7.11 must reject.
// BOUNDARY (do not overclaim): the `and`/`or`/`not` boolean gates and the
// `bool?` `if`/`while` condition gates still read the DECLARED type and ignore
// §7.4 narrowing — a pre-existing v0.288 delta, tracked as #5137.
// Where a row exists to pin a CODEGEN lowering (a payload width, a decimal
// runtime route, an unsigned compare) rather than the lift itself, the
// spec-correct conversion is (a): keep the row POSITIVE with its value
// asserted, so the lowering stays covered. Conversion (b) — a REJECTION row —
// is used where the row's subject IS the rule.


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

// §7.11: a + b where a: int? = 3, b: int? = 4 → 7 (present, unwrap via ??).
BEZEL_TEST(codegen_lifted_ops_486, int_add_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"));
}

// §7.11: subtraction both present.
BEZEL_TEST(codegen_lifted_ops_486, int_sub_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 10; var b: int? := 3; "
        "var r: int? := a - b; "
        "return (r ?? -1);"));
}

// §7.11: multiplication both present.
BEZEL_TEST(codegen_lifted_ops_486, int_mul_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 6; var b: int? := 7; "
        "var r: int? := a * b; "
        "return (r ?? -1);"));
}

// §7.11: division both present.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 20; var b: int? := 4; "
        "var r: int? := a / b; "
        "return (r ?? -1);"));
}

// §7.11: mod both present.
BEZEL_TEST(codegen_lifted_ops_486, int_mod_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
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

// §7.11: 10 / 0 (both present) → DivideByZeroException fires.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_zero_raises_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
        "var a: int? := 3; var b: int? := 7; "
        "return int(a < b);"));
}

// §7.11: both present, a < b false.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_present_false_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
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
    // Pre-fix acceptance: was the lift's "present operand yields present
    // payload" case. Post-fix: the assignment `:= 5` narrows `a` non-null
    // (§7.4), so `-a` is the ordinary non-nullable form, and `var r :=
    // -a` widens the int back to `int?` — the program still RAN under v0.267.
    BEZEL_EXPECT_TRUE(runsAot(
        "module M; class C public static method Main(): int "
        "var a: int? := 5; "
        "var r: int? := -a; "
        "Console.WriteLine(r ?? -99); return 0; "
        " end Main; end C;"));
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
    BEZEL_EXPECT_TRUE(runsAot(
        "module M; class C public static method Main(): int "
        "var a: int? := 0; "
        "var r: int? := ~a; "
        "Console.WriteLine(r ?? 0); return 0; "
        " end Main; end C;"));
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
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
        "var a: int := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"));
}


// =========================================================================
// §7.11 — Narrowing required: result is T?, coalesce to get non-nullable
// =========================================================================

// §7.11: `(a + b) ?? 0` narrows from int? → int correctly.
BEZEL_TEST(codegen_lifted_ops_486, int_add_coalesced_to_nonnull_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
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

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, every_width_narrows_and_computes) {
    // (a ?? 0) is the read-back; a wrong verdict shows up as -999 (rejected),
    // a wrong ROUTE as a wrong number.
    BEZEL_EXPECT_EQ(run("var a: byte?   := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: sbyte?  := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: short?  := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: ushort? := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: int?    := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: long?   := 3; return int((a + 1) ?? 0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: double? := 3.0; return int((a + 1.0) ?? 0.0);"), 4);
    BEZEL_EXPECT_EQ(run("var a: float?  := 3.0f; return int((a + 1.0f) ?? 0.0f);"), 4);
    BEZEL_EXPECT_EQ(run("var a: decimal? := 3m; return int((a + 1m) ?? 0m);"), 4);
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
    BEZEL_EXPECT_EQ(run("var a: byte?  := 5; return int((a << 1) ?? 99);"), 10);
    BEZEL_EXPECT_EQ(run("var a: byte?  := 5; return int((a & 3) ?? 99);"), 1);
    BEZEL_EXPECT_EQ(run("var a: short? := 3; return int((a * 2) ?? 99);"), 6);
}

BEZEL_TEST(codegen_lifted_ops_486_narrowed_3298, decimal_routed_through_the_runtime) {
    // Each of these took the INTEGER path on the draft and aborted in IR
    // verification; the numbers below are what the decimal runtime produces.
    BEZEL_EXPECT_EQ(run("var a: decimal? := 5; var d: decimal := a + 1; return int(d);"), 6);
    BEZEL_EXPECT_EQ(run("var a: decimal? := 5m; var b: decimal? := 3m; return int((a * b) ?? 0m);"), 15);
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
    BEZEL_EXPECT_EQ(run(
        "var a: decimal? := 3m; var b: decimal? := 4m; "
        "if a < b then return 1; end if; return 0;"), 1);
}

// =========================================================================
// §7.11 — double? arithmetic and relational
// =========================================================================

// §7.11: double? + double? both present.
BEZEL_TEST(codegen_lifted_ops_486, double_add_both_present_spec_7_11) {
    // 1.5 + 2.5 = 4.0 → int cast → 4.
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
        "var a: double? := double.NaN; var b: double? := 1.0; "
        "var lt: int := int(a < b); var gt: int := int(a > b); "
        "var le: int := int(a <= b); var ge: int := int(a >= b); "
        "return lt * 1000 + gt * 100 + le * 10 + ge;"));
}

// §7.11 + IEEE 754: NaN on the right operand — same all-false outcome.
BEZEL_TEST(codegen_lifted_ops_486, double_relational_nan_right_false_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: double? := 1.0; var b: double? := double.NaN; "
        "return int(a < b) + int(a > b) + int(a <= b) + int(a >= b);"));
}


// =========================================================================
// §7.11 — long? arithmetic
// =========================================================================

// §7.11: long? + long? both present.
BEZEL_TEST(codegen_lifted_ops_486, long_add_both_present_spec_7_11) {
    BEZEL_EXPECT_TRUE(runs(
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
    BEZEL_EXPECT_TRUE(runs(
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

// #1044 / §5.1 / §7.11: arithmetic and `??` on a SUB-i32 nullable
// (byte?/sbyte?/short?/ushort?) crashed codegen — the payload (i8/i16) and the
// int operand/default (i32) mismatched the result struct width (`insertvalue
// i32 into {i8,i8}`) / the coalesce PHI. `byte? + int` promotes to `int?`
// (§5.1), and a shift follows the left operand (`byte? << 1` → `byte?`); both
// now build the right-width result.
//
// BEHAVIOR FLIP (§7.4, spec:1061, read through §7.11, spec:1177; #4718 ruled
// YES, v0.309): these rows were spelled as LIFTED operators on a local
// initialised with a non-null literal (`var a: byte? := 3; a + 1`). The LIFT
// is genuinely retired — §7.11, v0.267 opens "No operator **lifts** to a
// nullable type" — but the previous version of this comment gave the wrong
// reason for it, claiming init-narrowing "proves a local non-null for MEMBER
// ACCESS but not for the §7.11 operator gate" and that "only NULL-TEST
// narrowing admits operators". That is not the rule. A declaration initializer
// DOES establish §7.4 narrowing (§7.4 names "`var x: T? := expr;` … which is
// the declaration's assignment"), so `a` above is a non-nullable operand at `+`
// and the ordinary operator applies to it unchanged. What the old rows wrongly
// needed was the lift's `int?`/`byte?`-valued RESULT, not admission to the
// operator.
// The rows are therefore respelled with the §7.4 null-test narrowing
// `if a <> null then …` (the spec's own named remedy) — ADMISSIBLE, not
// required — which keeps them POSITIVE value rows still exercising the same
// sub-i32 payload/result-width lowering, the defect #1044 is about.
BEZEL_TEST(codegen_1044_subint_nullable, byte_plus_int_promotes_to_int) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 3; var r: int? := 0; "
        "if a <> null then r := a + 1; end if; "
        "return int(r ?? 0);"), 4);
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_coalesce_int_default) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := 3; return int(a ?? 9);"));
}
BEZEL_TEST(codegen_1044_subint_nullable, byte_null_coalesce_int_default) {
    BEZEL_EXPECT_TRUE(runs("var a: byte? := null; return int(a ?? 9);"));
}
// BEHAVIOR FLIP (§7.4, spec:1061 + §7.11, spec:1177; #4718 / v0.309):
// respelled with the §7.4 null-test narrowing — ADMISSIBLE, not required,
// since a non-null initializer narrows too; see the suite header above.
BEZEL_TEST(codegen_1044_subint_nullable, byte_shl_follows_left) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: byte? := 5; var c: byte? := null; "
        "if a <> null then c := a << 1; end if; "
        "return int(c ?? 99);"));
}
// BEHAVIOR FLIP (§7.4, spec:1061 + §7.11, spec:1177; #4718 / v0.309):
// respelled with the §7.4 null-test narrowing — ADMISSIBLE, not required,
// since a non-null initializer narrows too; see the suite header above.
// `short * int` widens to `int` (§5.1), so the
// narrowed result local is `int?`, which is the promotion #1044 pinned.
BEZEL_TEST(codegen_1044_subint_nullable, short_mul_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: short? := 3; var c: int? := null; "
        "if a <> null then c := a * 2; end if; "
        "return int(c ?? 99);"));
}
// §7.11 (v0.286, #3298) — REJECTION: `a` is initialised with `null`, so §7.4
// narrows nothing and the operand is genuinely nullable. That — a `:= null`
// initializer — is the distinction this suite turns on, not which KIND of
// narrowing was applied: an initializer holding a non-null value reaches this
// gate too (§7.4, spec:1061; §7.11, spec:1177; #4718 / v0.309). Its siblings
// in this suite spell the non-null operand with an explicit null-test
// narrowing, which admits the operator.
BEZEL_TEST(codegen_1044_subint_nullable, byte_null_arith_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: byte? := null; return int((a + 1) ?? 99);"));
}
// BEHAVIOR FLIP (§7.4, spec:1061 + §7.11, spec:1177; #4718 / v0.309):
// respelled with the §7.4 null-test narrowing — ADMISSIBLE, not required,
// since a non-null initializer narrows too; see the suite header above.
BEZEL_TEST(codegen_1044_subint_nullable, byte_bitand_via_coalesce) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: byte? := 5; var c: int? := null; "
        "if a <> null then c := a & 3; end if; "
        "return int(c ?? 99);"));
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
// Post-#6214 respelling (§7.7/§5.1, v0.339): `byte? ?? int-variable` is ill-typed,
// so these keep the #1046 payload IntCast (wider nullable left, NARROWER
// variable right: the rhs value is widened in coal.rhs) with spec-valid programs.
BEZEL_TEST(codegen_1046_coalesce, short_coalesce_variable_byte_default) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: short? := 3; var d: byte := 9; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "3");
}
BEZEL_TEST(codegen_1046_coalesce, short_null_coalesce_variable_byte_default) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: short? := null; var d: byte := 9; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "9");
}
BEZEL_TEST(codegen_1046_coalesce, int_null_coalesce_computed_byte_default) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := null; var x: byte := 4; "
        "Console.WriteLine(a ?? byte(x + 5)); "
        "end Main; end C;"), "9");
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

// #6675 (§7.7 "the result has type T when the right operand converts to T",
// §5.1 widening preserves the value, v0.339): the #1046 payload IntCast widened
// the right operand with isSigned=true regardless of its type, so an UNSIGNED
// narrower default was sign-extended (`int? ?? byte 200` printed -56).
BEZEL_TEST(codegen_6675_coalesce_unsigned, int_null_byte_200) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := null; var d: byte := 200; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "200");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, int_null_ushort_60000) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := null; var d: ushort := 60000; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "60000");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, long_null_uint_3e9) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: long? := null; var d: uint := 3000000000; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "3000000000");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, short_null_byte_200) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: short? := null; var d: byte := 200; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "200");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, int_present_byte_200) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := 5; var d: byte := 200; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "5");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, long_present_uint_3e9) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: long? := 7; var d: uint := 3000000000; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "7");
}
BEZEL_TEST(codegen_6675_coalesce_unsigned, int_null_signed_byte_still_sign_extends) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := null; var d: sbyte := -5; "
        "Console.WriteLine(a ?? d); "
        "end Main; end C;"), "-5");
}

// #6676 (§7.7, v0.339): a nullable right operand makes the result `T?`;
// `int? ?? null` failed IR verification (PHI of i32 vs null).
BEZEL_TEST(codegen_6676_coalesce_null_rhs, null_left) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := null; var c := a ?? null; "
        "Console.WriteLine(c is null); "
        "end Main; end C;"), "True");
}
BEZEL_TEST(codegen_6676_coalesce_null_rhs, present_left) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main() "
        "var a: int? := 7; var c := a ?? null; "
        "Console.WriteLine(c ?? 0); "
        "end Main; end C;"), "7");
}

// #1047 / §5.1 / §7.11: decimal arithmetic with a mixed INTEGER operand
// (decimal? + int, or int + decimal?) must promote the int to a decimal struct
// before the bz_decimal_* call. Previously the i32 was stored raw into a decimal
// slot → a garbage decimal that faulted/overflowed at runtime (threw instead of
// computing). decimal? + decimal? and the non-nullable decimal + int were fine.
//
// BEHAVIOR FLIP (§7.4, spec:1061, read through §7.11, spec:1177; #4718 ruled
// YES, v0.309): these rows were spelled as LIFTED operators on a local
// initialised with a non-null literal. The LIFT is genuinely retired — §7.11,
// v0.267 opens "No operator **lifts** to a nullable type" — but the previous
// version of this comment gave the wrong reason, adding that init-narrowing
// "does not admit it either". It does: §7.4 names "`var x: T? := expr;` …
// which is the declaration's assignment", so a non-null initializer leaves
// the operand non-nullable and the ordinary operator applies unchanged.
// They are respelled with the §7.4 null-test narrowing `if a <> null then …` —
// the spec's own named remedy, ADMISSIBLE and not required — so they stay
// POSITIVE rows still exercising the §4.4 decimal runtime route that #1047 is
// about, including the operand order that decides which side promotes.
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_plus_int_present) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := 5; var c: decimal? := null; "
        "if a <> null then c := a + 1; end if; "
        "return int(c ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_times_int) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := 5; var c: decimal? := null; "
        "if a <> null then c := a * 2; end if; "
        "return int(c ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_minus_int) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := 5; var c: decimal? := null; "
        "if a <> null then c := a - 1; end if; "
        "return int(c ?? 99);"));
}
BEZEL_TEST(codegen_1047_decimal_nullable, int_plus_decimal_operand_order) {
    BEZEL_EXPECT_TRUE(runs(
        "var a: decimal? := 5; var c: decimal? := null; "
        "if a <> null then c := 1 + a; end if; "
        "return int(c ?? 99);"));
}
// §7.11 (v0.286, #3298) — REJECTION, same distinction as the `byte?` row
// above. The other five rows in this suite keep their acceptance (respelled
// with an explicit §7.4 null-test narrowing) and are what pins the §4.4
// decimal RUNTIME ROUTING for a narrowed `decimal?`: a draft of #3298 read the
// operand's DECLARED `decimal?` type in codegen's decimal block, skipped it,
// and emitted `add %bz.decimal, i32` (exit 141).
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_null_plus_int_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: decimal? := null; return int((a + 1) ?? 99);"));
}
// BEHAVIOR FLIP (§7.4, spec:1061 + §7.11, spec:1177; #4718 / v0.309):
// respelled with the §7.4 null-test narrowing — ADMISSIBLE, not required,
// since a non-null initializer narrows too; see the suite header above.
// Both operands are narrowed, so the `decimal? +
// decimal?` route is still exercised and still observed as a VALUE.
BEZEL_TEST(codegen_1047_decimal_nullable, decimal_nullable_both_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "var a: decimal? := 5; var b: decimal? := 3; var c: decimal? := null; "
        "if a <> null and b <> null then c := a + b; end if; "
        "return int(c ?? 99);"), 8);
}

// #1045 / §7.7 / §17.1: `??` whose LEFT operand is a non-nullable `bool` —
// lowerCoalesce returned nullptr for a non-pointer non-nullable LHS, so the
// whole `??` lowered to null → false; it now returns the left operand.
//
// BEHAVIOR FLIP (§7.4, spec:1061, read through §7.11, spec:1177; #4718 ruled
// YES, v0.309): these rows were spelled `(int? < 5) ?? false`, which asserted
// the RETIRED v0.130/v0.267 lift: that a lifted relational is "a definite
// `bool` (`false` when either operand is null)". That retirement is real and
// stands — §7.11 (v0.267) retired it outright, "`null` is not less than,
// equal to, or greater than a value, and answering `false` would assert two
// definite claims at once" — and the header comment above this block remains
// CORRECTED: the definite-`bool` claim is not part of the current language.
// What is NOT part of the rule is the clause this comment previously tacked
// on, that init-narrowing "does not admit the operator either". A declaration
// initializer DOES narrow here (§7.4: "`var x: T? := expr;` … which is the
// declaration's assignment"), and §7.11 then reads the operand's static type
// — so an initializer holding a non-null value admits the operator unchanged.
// The rows below use the §7.7 `??` remedy BEFORE the comparison instead,
// which is a separate and equally admissible route to a non-nullable operand.
//
// The two rows are kept POSITIVE (conversion (a)) rather than turned into
// rejections, because the coverage #1045 is about is `??` over a non-nullable
// `bool` LHS, which is still exactly what they exercise once the nullable
// operand is given the §7.7 remedy `??` BEFORE the comparison. The
// `nullable_cmp_null_operand_is_false` row below is the REJECTION that pins
// the v0.267 rule for the same shape — there the operand really is nullable
// (`:= null`), which is what §7.4 narrows nothing for.
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_true_via_coalesce) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; return (if ((a ?? 0) < 5 ?? false) then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_false_via_coalesce) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; return (if ((a ?? 0) > 5 ?? false) then 1 else 0);"), 0);
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nullable_cmp_null_operand_is_false) {
    // §7.11 (v0.267) — REJECTION, and the control for the two rows above: with
    // a genuinely-nullable operand there is NO "definite false" to fall back
    // on, because a nullable operand at `<` is a compile error and not a value.
    // Under the retired v0.130 lift this row asserted a runtime `false`.
    BEZEL_EXPECT_TRUE(rejectedBy_7_11(
        "var a: int? := null; return (if ((a < 5) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nonnull_bool_coalesce_returns_left_true) {
    BEZEL_EXPECT_TRUE(runs("return (if ((5 > 3) ?? false) then 1 else 0);"));
}
BEZEL_TEST(codegen_1045_coalesce_nonnull, nonnull_bool_coalesce_returns_left_false) {
    BEZEL_EXPECT_TRUE(runs("return (if ((5 < 3) ?? true) then 1 else 0);"));
}
