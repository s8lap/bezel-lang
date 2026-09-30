// Codegen — #1961 (§6.6 / §5.2): the char compound-assignment / increment
// write-back must apply the §5.2 code-point narrowing.
//
// Per §6.6 (spec v0.194, L809-L815) a compound assignment whose LHS type T is
// `char` and whose `x op e` result R is an integer type is `c := char(c OP e)`,
// so "for a `char` target the §5.2 code-point rules apply." §5.2 (L679):
// "Casting an integer to `char` requires the integer to fall in U+0000-U+10FFFF
// excluding the surrogate range; otherwise `OverflowException`." §4.5 (L526):
// "Surrogate code points (U+D800 to U+DFFF) are not valid `char` values."
//
// Before the fix, emitCompoundOp treated a `char` LHS as a plain unsigned i32
// and never applied the §5.2 narrowing, so `char(0x10FFFF) += 1` stored the
// invalid scalar 0x110000 (a type-invariant violation) and exited 0 instead of
// raising OverflowException. The explicit `char(n + 1)` cast path was correct,
// isolating the fault to the compound/increment write-back. This test pins that
// the write-back now enforces the identical §5.2 contract at every compound and
// increment call site — local, `++`, `*=`, the built-in Array<char> indexer,
// and a computed property — while an in-range operation still stores its value
// and ordinary (non-char) integer compound assignment is unaffected.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning `int(c)` (or the named int) as the exit code —
// used for the in-range cases whose result fits an 8-bit exit code.
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

// --- The exact repro: `char(0x10FFFF) += 1` must raise, not store 0x110000. ---
// §6.6: `c += 1` is `c := char(c + 1)`; §5.2: 0x110000 > U+10FFFF → Overflow.
BEZEL_TEST(codegen_1961_char_compound, plus_equals_above_max_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0x10FFFF); c += 1;"), 1);
}

// §4.5 / §5.2: crossing into the surrogate block raises. `char(0xD7FF) += 1`
// desugars to `char(0xD800)`, a surrogate code point that is not a valid char.
BEZEL_TEST(codegen_1961_char_compound, plus_equals_into_surrogate_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0xD7FF); c += 1;"), 1);
}

// §5.2 via `++`: `c++` at the max code point is `c := char(c + 1)` → Overflow.
BEZEL_TEST(codegen_1961_char_compound, increment_above_max_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0x10FFFF); c++;"), 1);
}

// fix #5150 (§6.6, v0.309): respelled `c *= 2` as `c := char(int(c) * 2)` —
// the previous test row relied on the spec-violating `c *=` admission and is a
// behaviour flip: §6.6 (v0.309) says "`c *= 2` and `c /= 2` are compile
// errors". The explicit `char(int(c) * 2)` form shows the same Overflow
// at 0x10FFFF * 2 = 0x21FFFE (outside U+10FFFF).
BEZEL_TEST(codegen_1961_char_compound, times_equals_above_max_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0x10FFFF); c := char(int(c) * 2);"), 1);
}

// §5.2 via `-=`: `c -= 1` at U+0000 desugars to `char(-1)`, below the range.
BEZEL_TEST(codegen_1961_char_compound, minus_equals_below_zero_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var c: char := char(0); c -= 1;"), 1);
}

// In-range compound assignment still stores the value: `'A' += 1` → 'B' (66).
BEZEL_TEST(codegen_1961_char_compound, plus_equals_in_range_stores_value) {
    BEZEL_EXPECT_EQ(run("var c: char := 'A'; c += 1; return int(c);"), 66);
}

// In-range `++` still stores the value: `'A'++` → 'B' (66).
BEZEL_TEST(codegen_1961_char_compound, increment_in_range_stores_value) {
    BEZEL_EXPECT_EQ(run("var c: char := 'A'; c++; return int(c);"), 66);
}

// §17.6 built-in Array<char> indexer compound: the same §5.2 check applies to
// `arr[i] += 1` (the collection-indexer write-back call site).
BEZEL_TEST(codegen_1961_char_compound, array_indexer_above_max_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "using var arr := new Array<char>(); arr.Add(char(0x10FFFF)); "
        "arr[0] += 1;"), 1);
}

// Array<char> indexer compound in range still stores the value: 'A' → 'B'.
BEZEL_TEST(codegen_1961_char_compound, array_indexer_in_range_stores_value) {
    BEZEL_EXPECT_EQ(run(
        "using var arr := new Array<char>(); arr.Add('A'); "
        "arr[0] += 1; return int(arr[0]);"), 66);
}

// §22.9 computed-property compound: `o.Ch += 1` reads via the getter, computes
// `char(c + 1)`, and writes via the setter — the same §5.2 check applies.
BEZEL_TEST(codegen_1961_char_compound, property_above_max_throws) {
    auto r = runAot(
        "module M; class C "
        "  public _c: char := char(0x10FFFF); "
        "  public property Ch: char get => _c; set => do _c := value; end; end; "
        "  public static method Main(): int "
        "    var o := new C(); "
        "    try o.Ch += 1; return 0; catch OverflowException => return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// CONTROL — the explicit cast form was already correct; keep it pinned so the
// compound path and the cast path stay in lock-step on the §5.2 contract.
BEZEL_TEST(codegen_1961_char_compound, explicit_cast_above_max_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var n: int := 0x10FFFF; var c: char := char(n + 1);"),
        1);
}

// CONTROL — ordinary (non-char) integer compound assignment must be unaffected
// by the new char branch: a `uint` value that is in range must NOT spuriously
// raise (regression guard for the "only char gets the extra check" invariant).
BEZEL_TEST(codegen_1961_char_compound, uint_compound_in_range_no_throw) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var u: uint := 4000000000; u += 1;"), 0);
}

// CONTROL — a plain `int` compound still computes normally (42), unchanged.
BEZEL_TEST(codegen_1961_char_compound, int_compound_still_correct) {
    BEZEL_EXPECT_EQ(run("var x: int := 40; x += 2; return x;"), 42);
}
