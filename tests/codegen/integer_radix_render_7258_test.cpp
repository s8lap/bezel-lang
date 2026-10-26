// Tests for #7258 — an integer's radix rendering (`ToString(radix)` /
// `Convert.ToString(n, radix)`) emitted the WRONG TEXT for an unsigned value
// and for any value above `Int32.MaxValue`.
//
// ─── The normative sentence (stdlib §9.1, v0.344) ─────────────────────────────
// "`ToString(radix)` emits the **lower-case** digits — a `-` for a negative
//  value followed by the magnitude's digits most significant first, a single
//  `0` for zero, never a leading zero otherwise — and takes the same radix
//  set, raising `ArgumentException` for any other. **The two are therefore
//  inverse: `T.Parse(n.ToString(radix), radix)` is `n` for every value of
//  every integer type and every admitted radix.** `Convert.ToString(value,
//  toBase)` (§2.3) emits the same text as `ToString(radix)`. The no-format
//  `ToString()` ... emits the text `ToString(10)` emits: `(-42).ToString()` is
//  `"-42"`."
//
// stdlib §2.3 (v0.344): "`ToString(value, toBase)` returns the text
// `value.ToString(toBase)` returns (§9.1)" — so the two sites must AGREE.
//
// ─── The two sites and the three defects ─────────────────────────────────────
// SITE A — `src/codegen/LlvmBackend_Exprs.cpp`, the primitive receiver's
//   `ToString(radix)`: the receiver was normalised with `CreateSExt(recv,
//   i32())` (a `byte` 200 became -56) and formatted with a SIGNED format
//   family (`%d`/`%x`/`%o`), so a `uint` 4000000000 rendered as -294967296
//   and a `ulong` maximum as -1.
// SITE B — `src/codegen/LlvmBackend_Calls.cpp`, `Convert.ToString(n, radix)`:
//   `CreateSExt(arg, i32())` NARROWED a 64-bit argument to 32 bits and the
//   format family was signed — three independent defects (signedness, width,
//   format family).
//
// The rows below pin: the unsigned values (site A and site B), a signed value
// above `Int32.MaxValue` (the WIDTH defect, on a signed type), a negative
// signed value at a non-decimal radix (the `-` + MAGNITUDE rule of §9.1), a
// non-decimal radix with its lower-case digits, the agreement of the two
// sites, and the §9.1 round trip.
//
// The CONTROLS (green on BOTH arms): the no-format `ToString()`, the decimal
// signed rows, and the non-decimal renderings of a positive `int`/`long`.
//
// NOTE on the case of the hex digits: §9.1 says "`ToString(radix)` emits the
// **lower-case** digits", and the existing corpus/test rows pin "ff" for
// 255.ToString(16) — so the magnitude of -42 is "2a" and the text is "-2a"
// (the issue body wrote "-2A", which contradicts the sentence it quotes).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The program's exit code: 1 = the row's expectation held.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// The program's stdout — the rendered TEXT itself, byte-for-byte.
std::string runOut(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ─── the UNSIGNED rows (site A) ──────────────────────────────────────────────

// stdlib §9.1: a `uint` renders its own value — 4000000000, not -294967296.
BEZEL_TEST(integer_radix_render_7258, uint_to_string_decimal_4000000000_7258) {
    // stdlib §9.1: the radix text is the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(u.ToString(10));"),
        std::string("4000000000\n"));
}

// stdlib §9.1: a `byte` renders its own value — 200, not -56 (the sign
// extension alone; the value is positive and fits in 8 bits).
BEZEL_TEST(integer_radix_render_7258, byte_to_string_decimal_200_7258) {
    // stdlib §9.1: the radix text is the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var b: byte := 200; "
        "Console.WriteLine(b.ToString(10));"),
        std::string("200\n"));
}

// stdlib §9.1: a `ushort` renders its own value — 40000, not -25536.
BEZEL_TEST(integer_radix_render_7258, ushort_to_string_decimal_40000_7258) {
    // stdlib §9.1: the radix text is the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var m: ushort := 40000; "
        "Console.WriteLine(m.ToString(10));"),
        std::string("40000\n"));
}

// stdlib §9.1: a `ulong` renders its own value — 18446744073709551615, not -1.
BEZEL_TEST(integer_radix_render_7258, ulong_to_string_decimal_max_7258) {
    // stdlib §9.1: the radix text is the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var g: ulong := 18446744073709551615ul; "
        "Console.WriteLine(g.ToString(10));"),
        std::string("18446744073709551615\n"));
}

// stdlib §9.1: a `byte` in radix 16 — "c8", not the sign-extended "ffffffc8".
BEZEL_TEST(integer_radix_render_7258, byte_to_string_hex_200_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "var b: byte := 200; "
        "Console.WriteLine(b.ToString(16));"),
        std::string("c8\n"));
}

// stdlib §9.1: a `ushort` in radix 16 — "9c40", not "ffff9c40".
BEZEL_TEST(integer_radix_render_7258, ushort_to_string_hex_40000_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "var m: ushort := 40000; "
        "Console.WriteLine(m.ToString(16));"),
        std::string("9c40\n"));
}

// stdlib §9.1: a `uint` in radix 16 — "ee6b2800" (CONTROL: the base arm
// already rendered these digits — it printed them with the signed format).
BEZEL_TEST(integer_radix_render_7258, uint_to_string_hex_4000000000_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(u.ToString(16));"),
        std::string("ee6b2800\n"));
}

// stdlib §9.1: a `ulong` maximum in radix 16 — "ffffffffffffffff" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, ulong_to_string_hex_max_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "var g: ulong := 18446744073709551615ul; "
        "Console.WriteLine(g.ToString(16));"),
        std::string("ffffffffffffffff\n"));
}

// ─── the SIGNED rows (site A) ────────────────────────────────────────────────

// stdlib §9.1: a negative signed value renders '-' then its MAGNITUDE's
// digits — "-2a", not the two's complement "ffffffd6".
BEZEL_TEST(integer_radix_render_7258, int_to_string_hex_negative_42_7258) {
    // stdlib §9.1: "a `-` for a negative value followed by the magnitude's
    // digits most significant first" — and the digits are the lower-case ones.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString(16));"),
        std::string("-2a\n"));
}

// stdlib §9.1: the same rule in radix 8 — "-52", not "37777777726".
BEZEL_TEST(integer_radix_render_7258, int_to_string_octal_negative_42_7258) {
    // stdlib §9.1: '-' plus the magnitude's digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString(8));"),
        std::string("-52\n"));
}

// stdlib §9.1: the same rule in radix 2 — "-101010", not the 64-bit two's
// complement.
BEZEL_TEST(integer_radix_render_7258, int_to_string_binary_negative_42_7258) {
    // stdlib §9.1: '-' plus the magnitude's digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString(2));"),
        std::string("-101010\n"));
}

// stdlib §9.1: the same rule for a `long` — "-2a".
BEZEL_TEST(integer_radix_render_7258, long_to_string_hex_negative_42_7258) {
    // stdlib §9.1: '-' plus the magnitude's digits.
    BEZEL_EXPECT_EQ(runOut(
        "var n: long := -42L; "
        "Console.WriteLine(n.ToString(16));"),
        std::string("-2a\n"));
}

// stdlib §9.1: a signed value above `Int32.MaxValue` renders its own value
// (CONTROL: the base arm rendered this one correctly at site A — the WIDTH
// defect was at site B).
BEZEL_TEST(integer_radix_render_7258, long_to_string_decimal_4000000000_7258) {
    // stdlib §9.1: the radix text is the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var n: long := 4000000000L; "
        "Console.WriteLine(n.ToString(10));"),
        std::string("4000000000\n"));
}

// stdlib §9.1: the same value in radix 16 (CONTROL).
BEZEL_TEST(integer_radix_render_7258, long_to_string_hex_4000000000_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "var n: long := 4000000000L; "
        "Console.WriteLine(n.ToString(16));"),
        std::string("ee6b2800\n"));
}

// ─── the CONTROL rows (green on BOTH arms) ───────────────────────────────────

// stdlib §9.1: "the no-format `ToString()` ... emits the text `ToString(10)`
// emits: `(-42).ToString()` is `"-42"`" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, int_to_string_noarg_negative_42_7258) {
    // stdlib §9.1: the no-format text of a negative signed value.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString());"),
        std::string("-42\n"));
}

// stdlib §9.1: the no-format `ToString()` of an UNSIGNED value emits the text
// `ToString(10)` emits (CONTROL — this path was already correct).
BEZEL_TEST(integer_radix_render_7258, uint_to_string_noarg_4000000000_7258) {
    // stdlib §9.1: the no-format text of an unsigned value.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(u.ToString());"),
        std::string("4000000000\n"));
}

// stdlib §9.1: the decimal signed row (CONTROL).
BEZEL_TEST(integer_radix_render_7258, int_to_string_decimal_negative_42_7258) {
    // stdlib §9.1: the decimal text of a negative signed value.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString(10));"),
        std::string("-42\n"));
}

// stdlib §9.1: a positive `int` in radix 16 — "ff" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, int_to_string_hex_255_7258) {
    // stdlib §9.1: the lower-case digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((255).ToString(16));"),
        std::string("ff\n"));
}

// stdlib §9.1: a positive `long` in radix 2 — "11111111" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, long_to_string_binary_255_7258) {
    // stdlib §9.1: the binary digits.
    BEZEL_EXPECT_EQ(runOut(
        "var n: long := 255L; "
        "Console.WriteLine(n.ToString(2));"),
        std::string("11111111\n"));
}

// ─── site B — `Convert.ToString(n, radix)` ───────────────────────────────────

// stdlib §2.3 + §9.1: `Convert.ToString(4000000000u, 10)` renders the value
// as the `long` parameter it binds (stdlib §2.3's `ToString(value: long,
// toBase: int)`) — 4000000000, not -294967296.
BEZEL_TEST(integer_radix_render_7258, convert_to_string_uint_4000000000_7258) {
    // stdlib §2.3: "ToString(value, toBase) returns the text
    // value.ToString(toBase) returns"; stdlib §9.1: the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(4000000000u, 10));"),
        std::string("4000000000\n"));
}

// stdlib §2.3 + §9.1: `Convert.ToString(4000000000L, 10)` — a SIGNED `long`
// above `Int32.MaxValue`: 4000000000, not the truncated -294967296. This is
// the WIDTH defect (it is not an unsignedness bug).
BEZEL_TEST(integer_radix_render_7258, convert_to_string_long_4000000000_7258) {
    // stdlib §2.3: the `long` overload; stdlib §9.1: the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(4000000000L, 10));"),
        std::string("4000000000\n"));
}

// stdlib §2.3 + §9.1: the same value in a VARIABLE — the base arm narrowed
// the 64-bit argument to 32 bits and emitted malformed IR (`sext i64 to
// i32`), so the compiler ABORTED on this program.
BEZEL_TEST(integer_radix_render_7258, convert_to_string_long_var_4000000000_7258) {
    // stdlib §2.3: the `long` overload; stdlib §9.1: the value's own digits.
    BEZEL_EXPECT_EQ(runOut(
        "var n: long := 4000000000L; "
        "Console.WriteLine(Convert.ToString(n, 10));"),
        std::string("4000000000\n"));
}

// stdlib §2.3 + §9.1: `Convert.ToString(4000000000L, 16)` — "ee6b2800".
BEZEL_TEST(integer_radix_render_7258, convert_to_string_long_hex_4000000000_7258) {
    // stdlib §9.1: the lower-case digits of the value's own magnitude.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(4000000000L, 16));"),
        std::string("ee6b2800\n"));
}

// stdlib §2.3 + §9.1: a negative value at site B renders '-' plus its
// magnitude's digits — "-2a", not "ffffffd6".
BEZEL_TEST(integer_radix_render_7258, convert_to_string_negative_hex_42_7258) {
    // stdlib §9.1: '-' plus the magnitude's digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(-42, 16));"),
        std::string("-2a\n"));
}

// stdlib §2.3 + §9.1: the same rule in radix 2 — "-101010".
BEZEL_TEST(integer_radix_render_7258, convert_to_string_negative_binary_42_7258) {
    // stdlib §9.1: '-' plus the magnitude's digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(-42, 2));"),
        std::string("-101010\n"));
}

// stdlib §9.1: a positive `int` at site B in radix 16 — "ff" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, convert_to_string_hex_255_7258) {
    // stdlib §9.1: the lower-case digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(255, 16));"),
        std::string("ff\n"));
}

// stdlib §9.1: a positive `int` at site B in radix 8 — "377" (CONTROL).
BEZEL_TEST(integer_radix_render_7258, convert_to_string_octal_255_7258) {
    // stdlib §9.1: the octal digits.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(Convert.ToString(255, 8));"),
        std::string("377\n"));
}

// ─── the two sites must AGREE (stdlib §2.3) ──────────────────────────────────

// stdlib §2.3: "`ToString(value, toBase)` returns the text `value.ToString
// (toBase)` returns" — the two sites agree on an unsigned value.
BEZEL_TEST(integer_radix_render_7258, sites_agree_uint_decimal_7258) {
    // stdlib §2.3: the two sites emit the same text.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(u.ToString(10)); "
        "Console.WriteLine(Convert.ToString(u, 10));"),
        std::string("4000000000\n4000000000\n"));
}

// stdlib §2.3: the two sites agree on a negative value in radix 16.
BEZEL_TEST(integer_radix_render_7258, sites_agree_negative_hex_7258) {
    // stdlib §2.3: the two sites emit the same text.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine((-42).ToString(16)); "
        "Console.WriteLine(Convert.ToString(-42, 16));"),
        std::string("-2a\n-2a\n"));
}

// ─── the §9.1 ROUND TRIP — the strongest form of the rule ───────────────────

// stdlib §9.1: "The two are therefore inverse: `T.Parse(n.ToString(radix),
// radix)` is `n` for every value of every integer type and every admitted
// radix." The base arm's text ("-294967296") is not even in `uint`'s range, so
// the round trip RAISED there.
BEZEL_TEST(integer_radix_render_7258, round_trip_uint_decimal_7258) {
    // stdlib §9.1: the inverse property.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(uint.Parse(u.ToString(10), 10));"),
        std::string("4000000000\n"));
}

// stdlib §9.1: the same round trip in radix 16.
BEZEL_TEST(integer_radix_render_7258, round_trip_uint_hex_7258) {
    // stdlib §9.1: the inverse property.
    BEZEL_EXPECT_EQ(runOut(
        "var u: uint := 4000000000u; "
        "Console.WriteLine(uint.Parse(u.ToString(16), 16));"),
        std::string("4000000000\n"));
}

// stdlib §9.1: the round trip of a negative value — the '-' + magnitude rule
// is exactly what makes the inverse property hold.
BEZEL_TEST(integer_radix_render_7258, round_trip_negative_hex_7258) {
    // stdlib §9.1: the inverse property.
    BEZEL_EXPECT_EQ(runOut(
        "Console.WriteLine(int.Parse((-42).ToString(16), 16));"),
        std::string("-42\n"));
}
