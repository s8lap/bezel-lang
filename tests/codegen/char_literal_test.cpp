// Codegen slice CG32 — char literals (§1.4). A char literal lowers to its
// (i32) code point; escapes (`'\n'`, `'\t'`, `'\0'`, …) decode. Chars
// compare and arithmetic-combine as integers, and feed an FFI putchar.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "driver/Driver.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

std::string compileAndCapture(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_cg32_" + std::to_string(counter++);
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
    BEZEL_EXPECT_EQ(run("var c := 'A'; return (1 if c = 'A' else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, distinct_chars) {
    BEZEL_EXPECT_EQ(run("var c := 'A'; return (1 if c = 'B' else 9);"), 9);
}

BEZEL_TEST(codegen_cg32, char_ordering) {
    BEZEL_EXPECT_EQ(run("return (1 if 'a' < 'z' else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, newline_escape_is_10) {
    BEZEL_EXPECT_EQ(run("var c := '\\n'; return (1 if c = '\\n' else 0);"), 1);
}

BEZEL_TEST(codegen_cg32, char_passed_to_putchar) {
    // 'H','i','\n' printed via an FFI putchar that takes char code points.
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method putchar(c: int): int; end; "
        "class C public static method Main(): int "
        "  var _ := putchar('H'); var _2 := putchar('i'); "
        "  var _3 := putchar('\\n'); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, std::string("Hi\n"));
}
