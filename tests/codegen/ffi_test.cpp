// Codegen slice CG26 — FFI / extern (§32). An extern block declares
// external C-ABI functions; calls to them lower to direct calls resolved
// against the host process (JIT) or libc (native link). This is the first
// observable stdout. JIT tests use libc functions with return values;
// a native-exe test captures real stdout.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Compile `source` to a temp exe and capture its stdout.
std::string compileAndCapture(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_cg26_" + std::to_string(counter++);
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

BEZEL_TEST(codegen_cg26, extern_call_returns_value) {
    // libc abs(-5) = 5, resolved against the host process by the JIT.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extern \"c\" public method abs(n: int): int; end; "
        "class C public static method Main(): int "
        "  return abs(0 - 5); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg26, extern_result_used_in_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extern \"c\" public method abs(n: int): int; end; "
        "class C public static method Main(): int "
        "  var a := abs(0 - 20); var b := abs(0 - 22); return a + b; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg26, putchar_writes_stdout) {
    // putchar prints 'H','i','\n' (72, 105, 10) — first real stdout.
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method putchar(c: int): int; end; "
        "class C public static method Main(): int "
        "  var _ := putchar(72); var _2 := putchar(105); "
        "  var _3 := putchar(10); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, std::string("Hi\n"));
}

// §2.3: Console is always available; Write/WriteLine are intrinsics lowered
// via POSIX write(2). These tests use compileAndCapture to observe stdout.

BEZEL_TEST(codegen_console, write_string) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.Write(\"Hello\"); return 0; "
        "end Main; end C;"), std::string("Hello"));
}

BEZEL_TEST(codegen_console, writeline_string) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"World\"); return 0; "
        "end Main; end C;"), std::string("World\n"));
}

BEZEL_TEST(codegen_console, writeline_int) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(42); return 0; "
        "end Main; end C;"), std::string("42\n"));
}

BEZEL_TEST(codegen_console, writeline_no_args) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(); return 0; "
        "end Main; end C;"), std::string("\n"));
}

BEZEL_TEST(codegen_console, writeline_interpolated) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  var x := 7; Console.WriteLine($\"x={x}\"); return 0; "
        "end Main; end C;"), std::string("x=7\n"));
}

BEZEL_TEST(codegen_console, write_bool) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.Write(true); return 0; "
        "end Main; end C;"), std::string("true"));
}

// §2.3 / stdlib §9: Convert.ToInt32 / Math.Abs / Math.Min / Math.Max
// as always-available built-in class method intrinsics.

BEZEL_TEST(codegen_convert, to_int32_from_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"42\"; return Convert.ToInt32(s); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_math, abs_positive) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int return Math.Abs(5); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_math, abs_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int return Math.Abs(0 - 7); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_math, min_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return Math.Min(3, 7) + Math.Max(3, 7); "
        "end Main; end C;"), 10);
}
