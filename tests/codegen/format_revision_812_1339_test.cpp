// Tests for the v0.172 formatting revision (spec §18.5, stdlib §2.18; #812
// item 1 + #1339). Covers, against the spec/stdlib contract:
//   - the specifier vocabulary: integer D/Dn, X/x/Xn, B/Bn; floating F/E/G/P/R;
//     temporal pattern letters (DateTime/DateOnly/TimeOnly), TimeSpan "c",
//     Guid "D"/"N";
//   - the double/float round-trip guarantee (no-format ToString() ≡ "R" ≡ the
//     shortest string that reparses to the identical value);
//   - FormatException (catchable) on an unrecognized specifier;
//   - user-type dispatch through the auto-imported IFormattable interface
//     (class and record receivers);
//   - the §18.5 sema rule: a format spec on a non-formattable operand
//     (bool/char/string/plain class) is a compile error; alignment-only holes
//     are unrestricted;
//   - the #1339 lexer/parser rules: `{{`/`}}` literal braces, top-level-only
//     `,`/`:` delimitation (nested `[...]` colon; nested string in a hole), and
//     a brace-free format spec (a `{` in the spec is a compile error).
//
// runJit runs Main() in-process and returns its int result; each program
// encodes pass/fail into that int.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// A program that fails the front end (parse/sema error) never runs.
bool hasCompileError(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
}
}  // namespace

// ── Integer specifiers: D/Dn, X/x/Xn, B/Bn (stdlib §2.18) ────────────────────
BEZEL_TEST(format_revision_812_1339, integer_D_X_B) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{7:D4}" = "0007" then ok := ok + 1; end;
  if $"{255:X}" = "FF" then ok := ok + 2; end;
  if $"{255:x}" = "ff" then ok := ok + 4; end;
  if $"{255:X4}" = "00FF" then ok := ok + 8; end;
  if $"{5:B}" = "101" then ok := ok + 16; end;
  if $"{5:B8}" = "00000101" then ok := ok + 32; end;
  return ok;
end Main; end C;
)BZ"), 63);
}

// B on a byte prints the natural-width bit pattern (255 → 8 ones).
BEZEL_TEST(format_revision_812_1339, binary_byte_natural_width) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var b: byte := 255;
  return 1 if $"{b:B}" = "11111111" else 0;
end Main; end C;
)BZ"), 1);
}

// ── Floating specifiers: F/Fn, E, G, P/Pn (stdlib §2.18) ─────────────────────
BEZEL_TEST(format_revision_812_1339, floating_F_P) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var amount := 1234.5;
  var ratio := 0.85;
  if $"{amount:F2}" = "1234.50" then ok := ok + 1; end;
  if $"{ratio:P1}" = "85.0%" then ok := ok + 2; end;
  if $"{amount:F}" = "1234.50" then ok := ok + 4; end;   // §2.18: F default = 2
  return ok;
end Main; end C;
)BZ"), 7);
}

// ── R round-trip on gnarly doubles; no-format ToString() ≡ R (round-trip) ─────
BEZEL_TEST(format_revision_812_1339, double_round_trip_shortest) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.1}" = "0.1" then ok := ok + 1; end;
  if $"{0.1 + 0.2}" = "0.30000000000000004" then ok := ok + 2; end;
  if $"{1.0 / 3.0}" = "0.3333333333333333" then ok := ok + 4; end;
  if $"{1e300}" = "1e+300" then ok := ok + 8; end;
  return ok;
end Main; end C;
)BZ"), 15);
}

// The no-format rendering and "R" produce the SAME string (the round-trip value).
BEZEL_TEST(format_revision_812_1339, noformat_equals_R) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var x := 1.0 / 3.0;
  var y := 0.1 + 0.2;
  return 3 if ($"{x}" = $"{x:R}") and ($"{y}" = $"{y:R}") else 0;
end Main; end C;
)BZ"), 3);
}

BEZEL_TEST(format_revision_812_1339, float_round_trip) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var f: float := 0.1f;
  return 3 if ($"{f}" = "0.1") and ($"{f:R}" = "0.1") else 0;
end Main; end C;
)BZ"), 3);
}

// ── Temporal patterns + TimeSpan "c" + Guid "D"/"N" (stdlib §2.5/§2.6/§2.18) ──
BEZEL_TEST(format_revision_812_1339, temporal_and_guid) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var d := new DateOnly(2024, 3, 15);
  if $"{d:yyyy-MM-dd}" = "2024-03-15" then ok := ok + 1; end;
  var t := new TimeOnly(13, 5, 9);
  if $"{t:HH:mm:ss}" = "13:05:09" then ok := ok + 2; end;
  var ts := TimeSpan.FromSeconds(3661.0);
  if $"{ts:c}" = "01:01:01" then ok := ok + 4; end;
  var g := Guid.Empty;
  if $"{g:D}" = "00000000-0000-0000-0000-000000000000" then ok := ok + 8; end;
  if $"{g:N}" = "00000000000000000000000000000000" then ok := ok + 16; end;
  return ok;
end Main; end C;
)BZ"), 31);
}

// ── FormatException (catchable) on an unrecognized specifier (§18.5/§2.18) ────
BEZEL_TEST(format_revision_812_1339, unrecognized_specifier_raises_catchable) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var x := 5;
  try
    var s := $"{x:QQ}";
    result := 99;
  catch FormatException => result := 7;
  end;
  return result;
end Main; end C;
)BZ"), 7);
}

// A recognized letter with a non-digit tail ("BOGUS" is NOT "B" + count 0) is
// an unrecognized specifier → FormatException, not a silent binary render.
BEZEL_TEST(format_revision_812_1339, recognized_letter_bad_tail_raises) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var x := 5;
  try
    var s := $"{x:BOGUS}";
    result := 99;
  catch FormatException => result := 3;
  end;
  return result;
end Main; end C;
)BZ"), 3);
}

BEZEL_TEST(format_revision_812_1339, unrecognized_timespan_specifier_raises) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var ts := TimeSpan.FromSeconds(1.0);
  try
    var s := $"{ts:Z}";
    result := 99;
  catch FormatException => result := 5;
  end;
  return result;
end Main; end C;
)BZ"), 5);
}

// ── User IFormattable dispatch — class receiver ──────────────────────────────
BEZEL_TEST(format_revision_812_1339, iformattable_class_dispatch) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M;
class Temp: IFormattable
  public V: int;
  public constructor(v: int)
    this.V := v;
  end constructor;
  public method ToString(format: string): string
    return $"<{format}={this.V}>";
  end ToString;
end Temp;
class C public static method Main(): int
  using var t := new Temp(7);
  return 1 if $"{t:C2}" = "<C2=7>" else 0;
end Main; end C;
)BZ"), 1);
}

// ── User IFormattable dispatch — record (value) receiver ─────────────────────
BEZEL_TEST(format_revision_812_1339, iformattable_record_dispatch) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M;
record Money: IFormattable
  public Amount: int;
  public method ToString(format: string): string
    return $"[{format}:{this.Amount}]";
  end ToString;
end Money;
class C public static method Main(): int
  var m := new Money(42);
  return 1 if $"{m:F2}" = "[F2:42]" else 0;
end Main; end C;
)BZ"), 1);
}

// ── §18.5 sema rule: a format spec requires a formattable operand ────────────
BEZEL_TEST(format_revision_812_1339, sema_error_format_spec_on_bool) {
    BEZEL_EXPECT_TRUE(hasCompileError(R"BZ(
module M; class C public static method Main(): int
  var b := true;
  Console.WriteLine($"{b:X}");
  return 0;
end Main; end C;
)BZ"));
}

BEZEL_TEST(format_revision_812_1339, sema_error_format_spec_on_char) {
    BEZEL_EXPECT_TRUE(hasCompileError(R"BZ(
module M; class C public static method Main(): int
  var ch := 'x';
  Console.WriteLine($"{ch:D2}");
  return 0;
end Main; end C;
)BZ"));
}

BEZEL_TEST(format_revision_812_1339, sema_error_format_spec_on_string) {
    BEZEL_EXPECT_TRUE(hasCompileError(R"BZ(
module M; class C public static method Main(): int
  var s := "hi";
  Console.WriteLine($"{s:F2}");
  return 0;
end Main; end C;
)BZ"));
}

BEZEL_TEST(format_revision_812_1339, sema_error_format_spec_on_plain_class) {
    BEZEL_EXPECT_TRUE(hasCompileError(R"BZ(
module M;
class Plain public V: int; end Plain;
class C public static method Main(): int
  using var p := new Plain();
  p.V := 1;
  Console.WriteLine($"{p:C2}");
  return 0;
end Main; end C;
)BZ"));
}

// A user type implementing IFormattable with a spec is NOT an error, and a
// bare alignment (no format) on a string is NOT an error.
BEZEL_TEST(format_revision_812_1339, sema_ok_iformattable_and_alignment) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M;
record Money: IFormattable
  public Amount: int;
  public method ToString(format: string): string
    return $"[{this.Amount}]";
  end ToString;
end Money;
class C public static method Main(): int
  var m := new Money(9);
  var s := "hi";
  var ok := 0;
  if $"{m:X}" = "[9]" then ok := ok + 1; end;
  if $"|{s,4}|" = "|  hi|" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── #1339 lexer/parser: literal braces, delimitation, brace-free spec ────────
BEZEL_TEST(format_revision_812_1339, literal_brace_doubling) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var x := 5;
  var ok := 0;
  if $"{{" = "{" then ok := ok + 1; end;
  if $"}}" = "}" then ok := ok + 2; end;
  if $"{{{x}}}" = "{5}" then ok := ok + 4; end;
  return ok;
end Main; end C;
)BZ"), 7);
}

// Top-level `:` delimits the format spec; a `:` inside `[...]` nests (the
// dictionary indexer's colon does not begin a format spec).
BEZEL_TEST(format_revision_812_1339, nested_bracket_colon) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  using var d := new Dictionary<string, double>();
  d["pi"] := 3.14159;
  return 1 if $"{d["pi"]:F2}" = "3.14" else 0;
end Main; end C;
)BZ"), 1);
}

// A `{` inside a format spec is a compile error (the spec is brace-free).
BEZEL_TEST(format_revision_812_1339, brace_in_format_spec_is_error) {
    BEZEL_EXPECT_TRUE(hasCompileError(R"BZ(
module M; class C public static method Main(): int
  var x := 5;
  Console.WriteLine($"{x:F{2}}");
  return 0;
end Main; end C;
)BZ"));
}
