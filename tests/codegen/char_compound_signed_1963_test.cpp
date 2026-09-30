// Codegen — #1963 (§6.6 / §6.2 / §5.2): a `char` compound assignment /
// increment must run its arithmetic SIGNED.
//
// Per §6.6 (spec v0.197) a compound assignment whose LHS type is `char` and
// whose `c op e` result is an integer is `c := char(c op e)`. The arithmetic
// `c op e` runs under §6.2 (mixed-type arithmetic): `char` implicitly converts
// to `int` (§5.1, L592), a SIGNED type, so `c op e` promotes the char to `int`
// and the operation is signed — exactly what the explicit `char(c op e)` cast
// computes. §5.2 (L679) then narrows the result back to `char`, raising
// OverflowException only when the result falls outside U+0000–U+10FFFF or in
// the surrogate range U+D800–U+DFFF.
//
// Before the fix, emitCompoundOp's char branch passed `uns` — which is
// isUnsignedPrim(char) == true — into the recursive arithmetic, so it emitted
// the UNSIGNED overflow intrinsic (uadd/usub.with.overflow). A negative RHS or
// intermediate then spuriously raised: `char(5) += -1` computed
// `uadd.with.overflow(5, 0xFFFFFFFF)`, which sets the overflow flag and raised
// OverflowException instead of yielding `char(5 + (-1)) = char(4)`. The
// explicit `char(n + -1)` cast was already correct (its `int` addition is
// signed), isolating the fault to the compound/increment arithmetic step.
//
// This test pins the SIGNED arithmetic at every char compound / increment call
// site (local, `++`/`--`, `*=`, the built-in Array<char> indexer, a computed
// property), AND that #1961's §5.2 code-point range check still fires on a
// genuinely out-of-range result — the two compose: signed arithmetic, then the
// §5.2 range check.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning `int(c)` (or the named int) as the exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- The exact repro: `char(5) += -1` is `char(5 + (-1)) = char(4)`. ---
// §6.2: the `int` addition is signed, so it does NOT overflow. FAILS WITHOUT
// the fix (unsigned uadd raised OverflowException → exit 200/70, not 4).
BEZEL_TEST(codegen_1963_char_signed, plus_equals_negative_computes_signed) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(5); c += -1; return int(c);"), 4);
}

// §6.2: `char(5) -= -1` is `char(5 - (-1)) = char(6)`. Signed usub on a
// negative RHS previously flagged overflow; now it computes 6.
BEZEL_TEST(codegen_1963_char_signed, minus_equals_negative_computes_signed) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(5); c -= -1; return int(c);"), 6);
}

// §6.2: subtracting toward zero with a negative RHS: `char(0) -= -1` = char(1).
BEZEL_TEST(codegen_1963_char_signed, minus_equals_negative_from_zero) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(0); c -= -1; return int(c);"), 1);
}

// §17.6 `--`: `'B'--` is `char('B' - 1) = 'A'` (65). The decrement's `1`
// operand and the char both promote to signed int.
BEZEL_TEST(codegen_1963_char_signed, decrement_computes_signed) {
    BEZEL_EXPECT_EQ(run("var c: char := 'B'; c--; return int(c);"), 65);
}

// §17.6 `++` in range still stores the value (control that the signed path
// keeps the ordinary positive case correct): `'A'++` → 'B' (66).
BEZEL_TEST(codegen_1963_char_signed, increment_positive_still_correct) {
    BEZEL_EXPECT_EQ(run("var c: char := 'A'; c++; return int(c);"), 66);
}

// fix #5150 (§6.6, v0.309): respelled `c *= 20` as `c := char(int(c) * 20)` —
// the previous test row relied on the spec-violating `c *=` admission and
// is a behaviour flip: §6.6 (v0.309) says "`c *= 2` and `c /= 2` are
// compile errors — `char` is not an arithmetic type, so a code point is
// advanced, never scaled". The explicit `char(int(c) * 20)` form is valid
// Bezel and produces the same result (3 * 20 = 60 = '<').
BEZEL_TEST(codegen_1963_char_signed, times_equals_in_range_signed) {
    BEZEL_EXPECT_EQ(run("var c: char := char(3); c := char(int(c) * 20); return int(c);"), 60);
}

// §17.6 Array<char> indexer compound with a negative RHS routes through the
// same signed char arithmetic: `arr[0] += -1` on char(5) → char(4).
BEZEL_TEST(codegen_1963_char_signed, array_indexer_negative_signed) {
    BEZEL_EXPECT_EQ(run(
        "using var arr := new Array<char>(); arr.Add(char(5)); "
        "arr[0] += -1; return int(arr[0]);"), 4);
}

// §22.9 computed-property compound with a negative RHS: read via getter,
// compute `char(5 + (-1))` signed, write via setter → 4.
BEZEL_TEST(codegen_1963_char_signed, property_negative_signed) {
    auto r = runAot(
        "module M; class C "
        "  public _c: char := char(5); "
        "  public property Ch: char get => _c; set => do _c := value; end; end; "
        "  public static method Main(): int "
        "    var o := new C(); o.Ch += -1; return int(o.Ch); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 4);
}

// --- #1961 §5.2 range check must still fire after the arithmetic goes signed. -
// A genuinely out-of-range result (above U+10FFFF) still raises: the two
// compose — signed arithmetic, THEN the §5.2 code-point narrowing.
BEZEL_TEST(codegen_1963_char_signed, above_max_still_raises) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0x10FFFF); c += 1;"), 1);
}

// §5.2: a result below U+0000 still raises — `char(0) -= 1` desugars to
// `char(-1)`, which the signed range check rejects (it was already raising, but
// for the wrong reason; pin that it still raises via the §5.2 check).
BEZEL_TEST(codegen_1963_char_signed, below_zero_still_raises) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c -= 1;"), 1);
}

// §4.5 / §5.2: crossing into the surrogate block still raises after the signed
// arithmetic: `char(0xD7FF) += 1` = char(0xD800), an invalid surrogate.
BEZEL_TEST(codegen_1963_char_signed, into_surrogate_still_raises) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0xD7FF); c += 1;"), 1);
}

// CONTROL — the explicit cast form was already correct; keep it pinned so the
// compound path and the cast path stay in lock-step on the signed arithmetic.
BEZEL_TEST(codegen_1963_char_signed, explicit_cast_negative_still_correct) {
    BEZEL_EXPECT_EQ(
        run("var n: int := 5; var c: char := char(n + -1); return int(c);"), 4);
}

// CONTROL — ordinary (non-char) unsigned integer compound assignment must be
// unaffected: a `uint` near its max still overflows on `+= 1` (regression guard
// that forcing the char branch signed did NOT weaken the uint overflow check).
BEZEL_TEST(codegen_1963_char_signed, uint_compound_overflow_unaffected) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var u: uint := 4294967295; u += 1;"), 1);
}
