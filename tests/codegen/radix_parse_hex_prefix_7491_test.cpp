// Regression tests for issue #7491 -- the radix parse ACCEPTED a `0x`/`0X`
// prefix at radix 16 and answered the prefixed value.
//
// stdlib §9.1 (v0.350), "The radix alphabet": `Parse(s, radix)` and
// `TryParse(s, radix, ...)` accept either case of the letter digits "under the
// input grammar the string forms carry throughout: an optional leading `+` or
// `-`, then one or more digits of that radix, and nothing else; no whitespace,
// no `0x`/`0b` prefix, no digit separators, and any other input raises
// `FormatException`" (TryParse: false). `x` is no digit of radix 16.
//
// C's strtoll/strtoull accept the prefix at base 16 and leave endptr at the
// NUL, so the endptr guard saw a clean parse. Measured pre-fix: Parse
// returned 31; TryParse returned true with v = 31.
//
// Oracle: runAot stdout of a program whose own `catch` prints FE.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

// stdout of `T.Parse(S, R)`: the value, or FE for FormatException.
std::string parseOut(const std::string& T, const std::string& s, int radix) {
    JitResult r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      Console.WriteLine(" + T + ".Parse(\"" + s + "\", " +
        std::to_string(radix) + "));\n"
        "    catch\n"
        "      FormatException => Console.WriteLine(\"FE\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    return r.ran ? r.stdoutText : std::string("<did not run>");
}

// stdout of `T.TryParse(S, R, out v)`: "<bool> <v>".
std::string tryParseOut(const std::string& T, const std::string& s, int radix) {
    JitResult r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var v: " + T + " := 0;\n"
        "    var ok := " + T + ".TryParse(\"" + s + "\", " +
        std::to_string(radix) + ", out v);\n"
        "    Console.WriteLine($\"{ok} {v}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    return r.ran ? r.stdoutText : std::string("<did not run>");
}

}  // namespace

BEZEL_TEST(radix_parse_hex_prefix_7491, int_parse_rejects_0x_prefix_9_1) {
    BEZEL_EXPECT_EQ(parseOut("int", "0x1F", 16), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0X1F", 16), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "+0x1F", 16), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "-0x1F", 16), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0x", 16), std::string("FE\n"));
}

BEZEL_TEST(radix_parse_hex_prefix_7491, every_integer_width_parse_rejects_0x_prefix_9_1) {
    for (const char* t : {"byte", "sbyte", "short", "ushort", "uint", "long"})
        BEZEL_EXPECT_EQ(parseOut(t, "0x1", 16), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("ulong", "0xFF", 16), std::string("FE\n"));
}

BEZEL_TEST(radix_parse_hex_prefix_7491, tryparse_rejects_0x_prefix_9_1) {
    BEZEL_EXPECT_EQ(tryParseOut("int", "0x1F", 16), std::string("False 0\n"));
    BEZEL_EXPECT_EQ(tryParseOut("int", "+0X1F", 16), std::string("False 0\n"));
    BEZEL_EXPECT_EQ(tryParseOut("long", "0xFF", 16), std::string("False 0\n"));
    BEZEL_EXPECT_EQ(tryParseOut("ulong", "0xFF", 16), std::string("False 0\n"));
    BEZEL_EXPECT_EQ(tryParseOut("byte", "0x1", 16), std::string("False 0\n"));
}

// The other radices already refused the prefix; the happy paths are intact,
// including "0b11" at radix 16, whose characters are all hex digits (0x0B11).
BEZEL_TEST(radix_parse_hex_prefix_7491, other_radices_and_happy_paths_unchanged_9_1) {
    BEZEL_EXPECT_EQ(parseOut("int", "0b1010", 2), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0x1", 10), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0x1F", 8), std::string("FE\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "ff", 16), std::string("255\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "FF", 16), std::string("255\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "-0f", 16), std::string("-15\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0b11", 16), std::string("2833\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "0", 16), std::string("0\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "1010", 2), std::string("10\n"));
    BEZEL_EXPECT_EQ(parseOut("int", "777", 8), std::string("511\n"));
    BEZEL_EXPECT_EQ(tryParseOut("int", "ff", 16), std::string("True 255\n"));
    BEZEL_EXPECT_EQ(tryParseOut("int", "0b11", 16), std::string("True 2833\n"));
}
