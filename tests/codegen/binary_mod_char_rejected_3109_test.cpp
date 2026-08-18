// Regression for #3109 — §6.1: binary `c mod 2` accepts a `char` operand while
// `c mod= 2` rejects it — the two spellings of one rule disagree.
//
// The asymmetry lives in ExprTypecheck_Exprs.cpp:4945 (binary `mod`) vs
// ExprTypecheck.cpp:11940 (`mod=`). §3.5's "Arithmetic" / "mod" row names
// integer types and `decimal` only — `char` is §4.5, a separate value type
// whose integer reading requires §5.2's explicit cast — so the spelling that
// rejects is the spelling that matches the spec. The compound `mod=` was already
// aligned (per #3052, the lane C fix shipped pre-#3109). This test pins the
// binary side on the same reading and adds the broad-vs-narrow controls the
// spec calls for: `int` and `decimal` remain admissible, `float`/`double`
// remain rejected, and `char` now rejects the binary form to match the compound
// form's existing posture.
//
// The diagnostic carrier: a compile error from `c mod 2` must reach
// `r.diagnostics.hasErrors()`; the rejection on `float mod 2.0` is the §6.1
// verbatim case (already there at the cited line); the `int`/`decimal`
// admissibility checks are value-level — they MUST run, not just compile.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

bool compileErrored(const std::string& body) {
    return runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;").diagnostics.hasErrors();
}

}  // namespace

// The bug: `c mod 2` on a `char` compiled pre-fix and produced a non-error
// exit (silent acceptance with implicit integer reading). The fix rejects this
// with a compile error citing §6.1.
// §3.5 ("Arithmetic" row, v0.254): "`mod` on integer types and `decimal`".
// §4.5: "`char` is a separate value type … explicit `int(c)` (§5.2) is the
// converted integer reading."
BEZEL_TEST(binary_mod_char_rejected_3109, char_left_operand_rejected_spec_3_5_4_5) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  var r: int := c mod 2; "
        "  return r;"));
}

// The fix is on the BINARY spelling; the RULE aligns both spellings.
// Compound `mod=` was already correct (per #3052); this re-asserts that the
// binary tightening did NOT regress the compound path.
BEZEL_TEST(binary_mod_char_rejected_3109, char_compound_mod_assign_unchanged_spec_6_6) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  c mod= 2; "
        "  return int(c);"));
}

// Broad-vs-narrow #1: `int mod int` MUST still be admissible (the whole
// asymmetric-reading argument assumed the integer case stayed legal).
BEZEL_TEST(binary_mod_char_rejected_3109, int_operands_still_admitted_spec_6_1) {
    BEZEL_EXPECT_EQ(run("var i: int := 47; var j: int := 10; return i mod j;"), 7);
}

// Broad-vs-narrow #2: `decimal mod decimal` MUST still be admissible (§6.1
// admits decimal on the second clause: "`mod` is defined on integer types and
// on `decimal`").
BEZEL_TEST(binary_mod_char_rejected_3109, decimal_operands_still_admitted_spec_6_1) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 47.5m; "
        "  var e: decimal := 10m; "
        "  Console.WriteLine(d mod e); "
        "  return 0; "
        "end Main; end C;"), "7.5");
}

// Broad-vs-narrow #3: `float`/`double` rejection (§6.1 verbatim) MUST still
// be in place; the tightening did not displace it.
BEZEL_TEST(binary_mod_char_rejected_3109, float_operand_rejected_spec_6_1) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var f: float := 47.0f; "
        "  return int(f mod 10.0f);"));
}

// Broad-vs-narrow #4: `double` on the SAME shoulder as `float` — §6.1 names
// the pair in a single clause.
BEZEL_TEST(binary_mod_char_rejected_3109, double_operand_rejected_spec_6_1) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var d: double := 47.5; "
        "  return int(d mod 10.0);"));
}

// The narrow form the issue staked out — a `char` value converted to `int`
// first (§5.2) is the canonical spelling the rationale points at; it MUST
// remain the well-typed path the rejection directs programs toward.
// §5.2: "`char` to integer types … the integer conversion succeeds and the
// result is the code-point value."
BEZEL_TEST(binary_mod_char_rejected_3109, char_via_explicit_cast_admitted_spec_5_2) {
    BEZEL_EXPECT_EQ(run(
        "  var c: char := 'a'; "
        "  return int(c) mod 2;"), 1);  // 'a' = 97 mod 2 = 1
}
