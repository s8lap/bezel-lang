// Codegen slice CG7 — bezelc emits a native executable. These are
// integration tests: they drive the real pipeline (front-end → LLVM
// object → clang link), then execute the produced binary and check its
// exit code. Requires clang on PATH (the project toolchain).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirCg7() {
    return wtScratchDir("wt-cg7");
}

// Compile `source` to a temp executable and run it; returns the exit
// code, or -1 if compilation/link failed.
int compileAndRun(const std::string& source) {
    auto dir = wtDirCg7();
    std::string stem = unique_stem("bezel_cg7");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();

    {
        std::ofstream out(src);
        out << source;
    }

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

} // namespace

BEZEL_TEST(codegen_cg7, expression_main_binary) {
    int code = compileAndRun(
        "module M; class C public static method Main(): int => 7; end C;");
    BEZEL_EXPECT_EQ(code, 7);
}

BEZEL_TEST(codegen_cg7, computed_binary) {
    int code = compileAndRun(
        "module Demo; class App "
        "  public static method Square(n: int): int => n * n; "
        "  public static method Main(): int "
        "    var x := Square(6); "
        "    if x > 30 then return x; end; "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_EQ(code, 36);
}

BEZEL_TEST(codegen_cg7, recursive_binary) {
    int code = compileAndRun(
        "module M; class C "
        "  public static method Fact(n: int): int "
        "    if n <= 1 then return 1; end; return n * Fact(n - 1); "
        "  end Fact; "
        "  public static method Main(): int => Fact(5); "
        "end C;");
    BEZEL_EXPECT_EQ(code, 120);
}
