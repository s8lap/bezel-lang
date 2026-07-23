// Accept controls for #2126 (§8.1 / §26.5 / stdlib §9): the fix that rejects a
// NONEXISTENT member on a primitive-typed value (see the reject track,
// tests/lit/primitive_member_no_member_reject_2126.bzl) must NOT over-reject the
// members primitives DO expose — the documented instance members (stdlib §9),
// the universal Object members (§8.1: ToString/Equals/GetHashCode), and §26.5
// extensions. Each test below compiles AND runs a program exercising a real
// primitive member and checks its runtime value, proving no over-reject and
// correct lowering (the same members that #2126 left silently untyped).
//
// runAot returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// stdlib §9.1: `int.ToString()` on a value returns the decimal string — "255"
// has length 3. (The universal §8.5 ToString on every primitive.)
BEZEL_TEST(primitive_member_2126, int_tostring_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 255; var s := x.ToString(); return s.Length; "   // "255" → 3
        "end Main; end C;"), 3);
}

// stdlib §9.1: `int.CompareTo(other): int` (IComparable<int>). 3.CompareTo(3)==0.
BEZEL_TEST(primitive_member_2126, int_compareto_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 3; var b := 3; var r := a.CompareTo(b); "
        "  return (if r = 0 then 11 else 22); "
        "end Main; end C;"), 11);
}

// §8.1 / stdlib §9.1: `int.Equals(other): bool` (auto IEquatable<int>).
// 5.Equals(5) is true → the taken branch returns 42.
BEZEL_TEST(primitive_member_2126, int_equals_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 5; var e := x.Equals(5); "
        "  return (if e then 42 else 7); "
        "end Main; end C;"), 42);
}

// §8.1: `int.GetHashCode(): int` — must still compile+run (was untyped/silent
// before the fix would have wrongly claimed no such member). Its exact value is
// unspecified; the test proves the program compiles and runs to a fixed result.
BEZEL_TEST(primitive_member_2126, int_gethashcode_compiles) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 5; var h := x.GetHashCode(); "
        "  return (if h = h then 9 else 0); "
        "end Main; end C;"), 9);
}

// stdlib §9.5: `char.CompareTo(other): int` (IComparable<char>). 'a' < 'b', so
// CompareTo is negative → the taken branch returns 13.
BEZEL_TEST(primitive_member_2126, char_compareto_ordering) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c := 'a'; var d := 'b'; var r := c.CompareTo(d); "
        "  return (if r < 0 then 13 else 0); "
        "end Main; end C;"), 13);
}

// §3.5: primitive comparison operators on `char` are unaffected — 'a' < 'b'.
BEZEL_TEST(primitive_member_2126, char_comparison_operator) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if 'a' < 'b' then 1 else 0); "
        "end Main; end C;"), 1);
}

// stdlib §9.1: the static `int.Parse(s): int` family is a DIFFERENT path
// (primitive-keyword static, §3.4) — guard it stays accepted alongside the
// value-receiver reject. int.Parse("42") → 42.
BEZEL_TEST(primitive_member_2126, int_parse_static) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := int.Parse(\"42\"); return n; "
        "end Main; end C;"), 42);
}

// §26.5 / §26.2: a user extension on a primitive receiver still resolves — the
// reject must be consulted AFTER extension lookup. `extension int { Doubled }`.
BEZEL_TEST(primitive_member_2126, int_extension_resolves) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int public method Doubled(): int => this * 2; end; "
        "class C public static method Main(): int "
        "  var x := 21; return x.Doubled(); "                          // 42
        "end Main; end C;"), 42);
}
