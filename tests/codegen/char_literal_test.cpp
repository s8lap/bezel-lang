// Codegen slice CG32 — char literals (§1.4). A char literal lowers to its
// (i32) code point; escapes (`'\n'`, `'\t'`, `'\0'`, …) decode. Chars
// compare and arithmetic-combine as integers, and feed an FFI putchar.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirCg32() {
    return wtScratchDir("wt-cg32");
}

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

std::string compileAndCapture(const std::string& source) {
    auto dir = wtDirCg32();
    std::string stem = unique_stem("bezel_cg32");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::string out;
    if (r.success) {
        if (FILE* pipe = popen(exe.c_str(), "r")) {
            std::array<char, 256> buf;
            while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
                out.append(buf.data(), n);
            pclose(pipe);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return out;
}

} // namespace

BEZEL_TEST(codegen_cg32, char_code_point) {
    BEZEL_EXPECT_EQ(run("var c := 'A'; return (if c = 'A' then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, distinct_chars) {
    BEZEL_EXPECT_EQ(run("var c := 'A'; return (if c = 'B' then 1 else 9);"), 9);
}

BEZEL_TEST(codegen_cg32, char_ordering) {
    BEZEL_EXPECT_EQ(run("return (if 'a' < 'z' then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, newline_escape_is_10) {
    BEZEL_EXPECT_EQ(run("var c := '\\n'; return (if c = '\\n' then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, char_passed_to_putchar) {
    // 'H','i','\n' printed via an FFI putchar that takes char code points.
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method putchar(c: int): int; end; "
        "class C public static method Main(): int "
        "  _ := putchar('H'); var _2 := putchar('i'); "
        "  var _3 := putchar('\\n'); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, std::string("Hi\n"));
}

// #1282 (§9.5 / §4.6): char.ToString() / char interpolation must preserve the
// full Unicode scalar, not truncate the code point to its low byte. Before the
// fix a char above U+00FF was rendered via snprintf `%c` (low byte only), so
// `char(0x141)` ('Ł') collided with `char(0x41)` ('A'). The char→string path now
// stores the full code point at the narrowest width. The rendered string's first
// code point round-trips back to the original char.
BEZEL_TEST(codegen_cg32, char_tostring_high_codepoint_roundtrips_1282) {
    // 'Ł' == U+0141 (321). s[0] read back as a char must equal char(321), not
    // char(321 & 0xFF)==char(65)=='A'. Return 1 on success.
    BEZEL_EXPECT_EQ(run(
        "var c := char(321); var s := c.ToString(); "
        "return (if s[0] = c then 1 else 0);"), 1);
}

// #1282: two chars whose code points differ only above the low byte must NOT
// produce equal strings (the truncation-collision the fix removes).
BEZEL_TEST(codegen_cg32, char_tostring_no_low_byte_collision_1282) {
    // char(321) low byte == 65 == 'A'. Their ToString()s must now differ.
    BEZEL_EXPECT_EQ(run(
        "var hi := char(321).ToString(); var lo := char(65).ToString(); "
        "return (if hi <> lo then 1 else 0);"), 1);
}

// #1282: a char above the BMP (U+1F600) round-trips its single code point.
BEZEL_TEST(codegen_cg32, char_tostring_astral_codepoint_roundtrips_1282) {
    BEZEL_EXPECT_EQ(run(
        "var c := char(128512); var s := c.ToString(); "
        "return (s.Length * 10 + (if s[0] = c then 1 else 0));"), 11);  // Length 1, s[0]==c
}
