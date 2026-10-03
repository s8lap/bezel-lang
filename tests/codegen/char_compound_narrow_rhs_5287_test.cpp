// Codegen — #5287 (§6.6, v0.313): a `char` compound target's right operand is
// "typed as in `int op e` under §6.2 — `sbyte`, `byte`, `short`, `ushort`,
// `int` or `long`, or an integer constant", so `c += b` over `b: byte` is
// `c := char(int(c) + int(b))` and is admitted.
//
// Before the fix all four narrow types were rejected with the §6.2/§6.6 pair
// diagnostic ("has no implicit conversion between operand types `char` and
// `byte`"), because none of them widens to `char` under §5.1 — a code point is
// not a widened byte — while the pair §6.6 actually types is `int op byte`,
// which §6.2 promotes. `int`/`long` right operands were admitted throughout
// (they do widen to `char`), which is why the gap went unnoticed.
//
// These rows assert the RUNTIME result of each admitted shape, not just that
// it compiles: a sign/zero-extension error in the §6.2 promotion or a
// truncation instead of §5.2's checked narrowing would still compile.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string run(const std::string& body) {
    auto r = runAot("module M; class C public static method Main(): int " + body +
                    " return 0; end Main; end C;");
    if (!r.ran) return "NORUN";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
}  // namespace

// --- the four admitted right-operand types (§6.6's `int op e` list) ---

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_rhs_advances) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: byte := byte(2); "
                        "c += b; Console.WriteLine(int(c));"),
                    std::string("52"));
}

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, sbyte_rhs_advances_signed) {
    // `sbyte(-1)` must SIGN-extend: §6.2 makes `int + sbyte` signed, so the
    // code point steps back one to 49 (not to a huge unsigned value).
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: sbyte := sbyte(-1); "
                        "c += b; Console.WriteLine(int(c));"),
                    std::string("49"));
}

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, short_rhs_advances) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: short := short(2); "
                        "c += b; Console.WriteLine(int(c));"),
                    std::string("52"));
}

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, ushort_rhs_advances_unsigned) {
    // `ushort(65535)` must ZERO-extend: 50 + 65535 = 65585, a valid code point.
    // A sign-extended read would compute 50 - 1 = 49 and store char 49.
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: ushort := ushort(65535); "
                        "c += b; Console.WriteLine(int(c));"),
                    std::string("65585"));
}

// `-=` takes the same admitted list (§6.6: "a `char` target admits only `+=`
// and `-=`"), so the fix is not `+=`-only.
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_rhs_recedes) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: byte := byte(7); "
                        "c -= b; Console.WriteLine(int(c));"),
                    std::string("43"));
}

// --- §5.2's checked write-back still applies to the newly admitted types ---

// §6.6's write-back is a §5.2 CHECKED narrowing: "a result outside `char`'s
// range raises OverflowException ... never a silent truncation".
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_rhs_over_upper_bound_raises) {
    BEZEL_EXPECT_EQ(run("var c: char := char(1114111); var b: byte := byte(1); "
                        "try c += b; Console.WriteLine(\"NORaise\"); "
                        "catch OverflowException => "
                        "Console.WriteLine(\"OVF-upper\"); end;"),
                    std::string("OVF-upper"));
}

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, short_rhs_under_lower_bound_raises) {
    // 50 + (-51) = -1, below char's lower bound 0.
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: short := short(-51); "
                        "try c += b; Console.WriteLine(\"NORaise\"); "
                        "catch OverflowException => "
                        "Console.WriteLine(\"OVF-lower\"); end;"),
                    std::string("OVF-lower"));
}

// The in-range edge of the same shape stores exactly (char(0), not a raise).
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, short_rhs_at_lower_bound_stores) {
    BEZEL_EXPECT_EQ(run("var c: char := char(50); var b: short := short(-50); "
                        "c += b; Console.WriteLine(int(c));"),
                    std::string("0"));
}

// --- the §6.6 rejections the fix must NOT widen (control rows) ---

// §6.6: "a `char` right operand is not arithmetic either (`c -= d` over
// `d: char` is rejected ...)".
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, char_rhs_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(50); var d: char := char(3); "
                    "c -= d; return int(c); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "operator `-` is not defined for `char` operands"));
}

// §6.6: "a non-constant `uint` or `ulong` operand has no §6.2 promotion with
// `int` and is a compile error" — the dedicated §6.6 branch's own wording.
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, uint_rhs_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(50); var u: uint := uint(2); "
                    "c += u; return int(c); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "compound assignment `+=` on a `char` target has no `uint` right operand"));
}

// §6.6: "`c *= 2`, `c /= 2` and `c mod= n` are compile errors — `char` is not
// an arithmetic type, so a code point is advanced, never scaled" (#5150).
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_rhs_times_is_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(50); var b: byte := byte(2); "
                    "c *= b; return int(c); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "operator `*` is not defined for `char` operands"));
}

// A bitwise form on a `char` target has no R (§6.6) — the #2155 bitwise
// pair check is a separate branch and is untouched by this fix.
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_rhs_bitwise_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(50); var b: byte := byte(2); "
                    "c &= b; return int(c); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "compound assignment `&=` has no implicit conversion between operand types `char` and `byte`"));
}

// The #2155 pair rejection that this fix exempts ONLY for a `char` target is
// unchanged for an integer target: `i += u` over `i: int`, `u: uint` has no
// §6.2 promotion either way (§5.1: never implicit between signed and unsigned).
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, int_target_uint_rhs_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var i: int := 50; var u: uint := uint(2); "
                    "i += u; return i; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "compound assignment `+=` has no implicit conversion between operand types `int` and `uint`"));
}

// The §6.6 narrow-integer write-back for a NON-char target is untouched:
// `b += 10` on `b: byte` still stores the checked §5.2 narrowing.
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, byte_target_int_rhs_writeback_control) {
    BEZEL_EXPECT_EQ(run("var b: byte := byte(50); b += 2; "
                        "Console.WriteLine(int(b));"),
                    std::string("52"));
}

// --- §6.6 "Nullable targets and operands" (control rows for this lane) ---
//
// §6.6: a read-modify-write compound "is a compile error when the static type
// of its left-hand side, OR OF ITS RIGHT OPERAND, is a nullable type — either
// position". #5287 admits the §6.6 right-operand LIST, which is spelled with
// non-nullable types; an UN-narrowed `byte?` operand and an un-narrowed
// `char?` target both stayed rejected before this lane and must still be.
//
// "Static type is read as §7.4 reads it" (§6.6) — a `byte?` §7.4-narrowed
// non-null at the statement IS a non-nullable operand, so `var b: byte? :=
// byte(2); c += b;` is the admitted `byte` shape, not a nullable one. These
// two rows use the un-narrowed (`= null`) spellings, which are what §6.6's
// nullable rule actually rejects.
BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, nullable_rhs_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char := char(50); var b: byte? := null; "
                    "c += b; return int(c); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "compound assignment `+=` is not defined when the right-hand operand is nullable (`byte?`)"));
}

BEZEL_TEST(codegen_5287_char_compound_narrow_rhs, nullable_char_target_still_rejected) {
    auto r = runAot("module M; class C public static method Main(): int "
                    "var c: char? := null; var b: byte := byte(2); "
                    "c += b; return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "compound assignment `+=` is not defined when the target is nullable (`char?`)"));
}
