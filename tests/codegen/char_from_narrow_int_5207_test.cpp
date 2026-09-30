// Codegen — #5207 (§5.2, v0.313): an integer converts to `char` when it lies
// in U+0000-U+10FFFF excluding the surrogates U+D800-U+DFFF, otherwise
// OverflowException. Sources narrower than the 32-bit char (byte, sbyte,
// short, ushort) used to emit `trunc i8 to i32` (IR verification failure), and
// unsigned narrow sources must zero-extend (byte 200, ushort 0xFFFF are valid).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Prints int(char(<T>(v))) ; "OVF" when OverflowException is raised.
std::string conv(const std::string& ty, const std::string& lit) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var v: " + ty + " := " + lit + "; "
        "  try var c: char := char(v); Console.WriteLine(int(c)); "
        "  catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "  return 0; end Main; end C;");
    if (!r.ran) return "NORUN";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

// The issue's exact repro.
BEZEL_TEST(codegen_5207_char_from_narrow, byte_repro) {
    BEZEL_EXPECT_EQ(conv("byte", "65"), std::string("65"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, byte_high_bit_is_valid) {
    BEZEL_EXPECT_EQ(conv("byte", "200"), std::string("200"));
    BEZEL_EXPECT_EQ(conv("byte", "255"), std::string("255"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, sbyte) {
    BEZEL_EXPECT_EQ(conv("sbyte", "65"), std::string("65"));
    BEZEL_EXPECT_EQ(conv("sbyte", "-1"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("sbyte", "-128"), std::string("OVF"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, short_) {
    BEZEL_EXPECT_EQ(conv("short", "1000"), std::string("1000"));
    BEZEL_EXPECT_EQ(conv("short", "32767"), std::string("32767"));
    BEZEL_EXPECT_EQ(conv("short", "-1"), std::string("OVF"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, ushort) {
    BEZEL_EXPECT_EQ(conv("ushort", "65"), std::string("65"));
    BEZEL_EXPECT_EQ(conv("ushort", "65535"), std::string("65535"));
    BEZEL_EXPECT_EQ(conv("ushort", "55295"), std::string("55295"));   // 0xD7FF
    BEZEL_EXPECT_EQ(conv("ushort", "55296"), std::string("OVF"));     // 0xD800
    BEZEL_EXPECT_EQ(conv("ushort", "57343"), std::string("OVF"));     // 0xDFFF
    BEZEL_EXPECT_EQ(conv("ushort", "57344"), std::string("57344"));   // 0xE000
}
BEZEL_TEST(codegen_5207_char_from_narrow, int_) {
    BEZEL_EXPECT_EQ(conv("int", "1114111"), std::string("1114111"));
    BEZEL_EXPECT_EQ(conv("int", "1114112"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("int", "-1"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("int", "55296"), std::string("OVF"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, uint_) {
    BEZEL_EXPECT_EQ(conv("uint", "66"), std::string("66"));
    BEZEL_EXPECT_EQ(conv("uint", "1114111"), std::string("1114111"));
    BEZEL_EXPECT_EQ(conv("uint", "1114112"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("uint", "4000000000"), std::string("OVF"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, long_) {
    BEZEL_EXPECT_EQ(conv("long", "67"), std::string("67"));
    BEZEL_EXPECT_EQ(conv("long", "-1"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("long", "4294967361"), std::string("OVF"));
}
BEZEL_TEST(codegen_5207_char_from_narrow, ulong_) {
    BEZEL_EXPECT_EQ(conv("ulong", "68"), std::string("68"));
    BEZEL_EXPECT_EQ(conv("ulong", "1114112"), std::string("OVF"));
    BEZEL_EXPECT_EQ(conv("ulong", "18446744073709551615"), std::string("OVF"));
}
// Reverse direction: char -> narrow integers must not have the mirror bug.
BEZEL_TEST(codegen_5207_char_from_narrow, reverse_char_to_narrow) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var c: char := char(65); "
        "  var b: byte := byte(c); var s: short := short(c); "
        "  var u: ushort := ushort(c); "
        "  Console.WriteLine(int(b) + int(s) + int(u)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran, true);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("195\n"));
}

// Reverse direction values above the narrow target's range must raise, and
// high-bit-valued chars must round-trip (§5.2, v0.313).
BEZEL_TEST(codegen_5207_char_from_narrow, reverse_char_to_narrow_ranges) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var c: char := char(200); Console.WriteLine(int(byte(c))); "
        "  var d: char := char(40000); Console.WriteLine(int(ushort(d))); "
        "  try Console.WriteLine(int(short(d))); "
        "  catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "  var e: char := char(300); "
        "  try Console.WriteLine(int(byte(e))); "
        "  catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran, true);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n40000\nOVF\nOVF\n"));
}

// §7.x nullable cast form (spec line "The general rule", v0.313): `char?(x)`
// over a nullable narrow source converts a present value like `char(v)` and
// maps null to null; the non-nullable form on a present value converts too.
BEZEL_TEST(codegen_5207_char_from_narrow, nullable_sources) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 200; var b: byte? := null; var s: sbyte? := -1; "
        "  var ca: char? := char?(a); var cb: char? := char?(b); "
        "  Console.WriteLine(int(ca!)); "
        "  if cb = null then Console.WriteLine(\"null\"); end; "
        "  try var cs: char? := char?(s); if cs = null then Console.WriteLine(\"null\"); end; "
        "  catch OverflowException => Console.WriteLine(\"OVF\"); end; "
        "  Console.WriteLine(int(char(a!))); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran, true);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\nnull\nOVF\n200\n"));
}
