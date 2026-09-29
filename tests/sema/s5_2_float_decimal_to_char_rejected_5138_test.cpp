// fix #5138 / §5.2 (v0.309): a `float`, `double`, or `decimal` source does
// not convert to `char` — a code point, not a number, exactly as for `bool`
// (§5.2, v0.307/#4724's bool-to-char rejection). Per §5.2 (v0.309):
// "Casting an integer to `char` requires the integer to fall in
// U+0000–U+10FFFF excluding the surrogate range; otherwise `OverflowException`.
// A `float`, `double`, or `decimal` source does not convert to `char` — a
// code point, not a number, as for `bool` above (nor is `char` an arithmetic
// operand, §3.5) — so `char(x)` over one is a compile error; `char(int(x))`
// writes the truncation toward zero and the code-point check as the two
// conversions they are."
//
// Before this fix, `char(65.5)` compiled: the #4724/#4801 constant-fold arms
// treated `char` as an integral TARGET (the `toIntegral = tgtIsNarrowerInt ||
// pt == PrimitiveType::Char` disjunct) and only rejected a float/double/
// decimal-to-char conversion when the folded, truncated value was OUT OF
// RANGE — so any operand whose truncation lands in U+0000-U+10FFFF (e.g.
// 65.5 -> 65 -> 'A') was silently ADMITTED, spec-ahead-of-impl. This fix
// rejects the conversion UNCONDITIONALLY (constant or not), both for the
// non-nullable `char(x)` form (typeCall) and the nullable `char?(x)` form
// (checkNullableCastConstantOperand), before the constant-fold arms run —
// and removes the now-dead `|| Char` disjuncts from both copies of
// `toIntegral`.
//
// Behaviour-flip sweep (gtest + lit + tests/conformance): `git grep -nE
// "char\\("` across tests/, tests/lit, tests/conformance turned up only
// integer-sourced and char-sourced `char(...)` conversions (code-point
// literals/hex, `char(n + 1)`, `char(c op e)` compound-assignment desugars,
// `char(int(x))` — already the *idiomatic* spelling this issue directs
// users to) — NONE convert a float/double/decimal source. No existing row
// flips.
//
// Authority: spec §5.2, v0.309.

#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagWithAll(const bezel::test::SemaResult& r,
                    std::initializer_list<std::string_view> needles) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (auto n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

// The unique needle of this fix's diagnostic — distinguishes it from the
// #4724/#4801 constant-overflow diagnostics (which mention "constant" and
// "OverflowException", not this unconditional rejection's wording).
constexpr std::string_view kNeedle =
    "does not convert to `char` — a code point, not a number";
constexpr std::string_view kSpelling = "char(int(x))";

const char* kHeader =
    "module M; class C public static method Main(): int ";
const char* kFooter = " return 0; end Main; end C;";

bezel::test::SemaResult check(const std::string& body) {
    return parseSemaAll(std::string(kHeader) + body + kFooter);
}

}  // namespace

// =====================================================================
// REJECT — every float/double/decimal source, char and char? targets.
// =====================================================================

// The issue's exact repro: `char(65.5)` — a double literal that TRUNCATES
// into range (65 -> 'A'). Before the fix this was silently admitted.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_double_literal_in_range_rejected) {
    auto r = check("var c: char := char(65.5);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// `float` literal source (`f` suffix).
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_float_literal_rejected) {
    auto r = check("var c: char := char(65.5f);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// `decimal` literal source (`m` suffix).
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_decimal_literal_rejected) {
    auto r = check("var c: char := char(65.5m);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// A `double` LOCAL (non-constant operand) — proves the rejection is
// unconditional, not merely the constant-fold arm's range check.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_double_local_rejected) {
    auto r = check("var d: double := 65.5; var c: char := char(d);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// The nullable-target form `char?(x)`.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           nullable_char_of_double_rejected) {
    auto r = check("var d: double := 65.5; var c: char? := char?(d);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// An out-of-range double still gets THIS diagnostic, not the (now
// unreachable) constant-overflow one — the unconditional check runs first.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_out_of_range_double_gets_unconditional_diag) {
    auto r = check("var c: char := char(1.0e30);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {kNeedle, kSpelling}));
}

// =====================================================================
// ACCEPT — controls that §5.2 still permits. runAot VALUE rows: compile,
// run, and compare exact stdout.
// =====================================================================

// The spec's own idiomatic spelling: `char(int(x))` truncates then applies
// the code-point check as the two conversions they are. int(65.5) == 65.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_int_of_double_accepted) {
    auto r = runAot(std::string(kHeader) +
                    "var c: char := char(int(65.5)); "
                    "Console.WriteLine(c);" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\n"));
}

// int -> char, still permitted (integer source, §5.2's own example).
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_int_accepted) {
    auto r = runAot(std::string(kHeader) +
                    "var n: int := 65; var c: char := char(n); "
                    "Console.WriteLine(c);" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\n"));
}

// char -> char (identity conversion via a char source), unaffected.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_char_accepted) {
    auto r = runAot(std::string(kHeader) +
                    "var c0: char := char(65); var c: char := char(c0); "
                    "Console.WriteLine(c);" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\n"));
}

// char -> double, the OTHER direction §5.2 still permits ("I(c) for char c
// and any integer type I" generalizes to floats via widening semantics;
// spec: "double(m) ... round to the nearest representable binary value").
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           double_of_char_accepted) {
    auto r = runAot(std::string(kHeader) +
                    "var c: char := char(65); var d: double := double(int(c)); "
                    "Console.WriteLine(d);" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("65\n"));
}

// int -> char with a NON-constant operand still runs the runtime
// code-point range check (unaffected by this fix's control-flow ordering):
// the conversion compiles and raises OverflowException at run time.
BEZEL_TEST(s5_2_float_decimal_to_char_rejected_5138,
           char_of_int_runtime_overflow_unaffected) {
    auto r = runAot(std::string(kHeader) +
                    "var n: int := 0x110000; "
                    "try Console.WriteLine(char(n)); "
                    "catch OverflowException => "
                    "Console.WriteLine(\"caught OverflowException\"); "
                    "end;" + kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("caught OverflowException\n"));
}
