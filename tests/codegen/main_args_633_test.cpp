// Codegen — §33.4 / §33.6 command-line arguments for Main(args: Array<string>).
//
// Issue #633: Main(args: Array<string>) segfaulted on any access to `args`
// (including args.Count) because the C-entry shim declared `main` with the
// Bezel-typed signature `i32(ptr args)` instead of the C ABI signature
// `i32(i32 argc, ptr argv)`.  The OS passes argc as an i32 and argv as a ptr;
// if the function is declared `i32(ptr)` the CPU places argc (e.g. 1) in the
// pointer register, so the first dereference of args → SIGSEGV.
//
// Fix: declare the LLVM entry point `main(i32 argc, ptr argv)` and build the
// Array<string> from argv in the function prolog via bz_args_array.
//
// Spec derivation:
//   §33.2 — accepted Main signatures including Main(args: Array<string>)
//   §33.4 — args contains argv[1..argc-1]; argv[0] (program name) excluded
//   §33.6 — the canonical HelloWorld example using args
//
// All tests use the native compile+run harness (AOT binary) so the real OS
// argc/argv are in play.  JIT cannot pass argv from the test process, so the
// args coverage is AOT-only.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir633() {
    return wtScratchDir("wt-633");
}

// Compile `source` to a native executable and return the exe path, or ""
// on failure.  Caller is responsible for removing the exe.
std::string compileToExe(const std::string& source, const std::string& stem) {
    auto dir = wtDir633();
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    if (!r.success) { std::remove(exe.c_str()); return ""; }
    return exe;
}

// Run `exe` with `args` as argv[1..], return {exitCode, stdout}.
// exitCode = -1 on fork/exec failure; stdout captured via pipe.
struct RunResult { int exitCode; std::string out; };

RunResult runWithArgs(const std::string& exe,
                      const std::vector<std::string>& args) {
    // Build argv: exe, args..., null
    std::vector<const char*> argv;
    argv.push_back(exe.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // Use popen-style: construct the shell command with quoted args.
    // Quote each arg with single-quotes (sufficient for test strings).
    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) cmd += " '" + a + "'";
    cmd += " 2>/dev/null";

    RunResult res{-1, ""};
    if (FILE* pipe = popen(cmd.c_str(), "r")) {
        std::array<char, 512> buf;
        while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
            res.out.append(buf.data(), n);
        int st = pclose(pipe);
        if (st != -1 && WIFEXITED(st)) res.exitCode = WEXITSTATUS(st);
    }
    return res;
}

// Compile `source` to exe, run with `args`, remove exe; return run result.
RunResult compileAndRun(const std::string& source,
                        const std::vector<std::string>& args = {},
                        const std::string& stem = "") {
    std::string s = stem.empty()
        ? ("bezel_633_" + unique_stem(""))
        : stem;
    std::string exe = compileToExe(source, s);
    if (exe.empty()) return {-1, ""};
    auto res = runWithArgs(exe, args);
    std::remove(exe.c_str());
    return res;
}

} // namespace

// ---------------------------------------------------------------------------
// §33.4 / §33.6: args.Count == 0 when invoked with no user arguments
// ---------------------------------------------------------------------------
// §33.4: "args contains the command-line arguments; the program's own
// executable path is not included."
BEZEL_TEST(codegen_main_args_633, count_zero_when_no_args_spec_33_4) {
    auto res = compileAndRun(
        "module M; class C "
        "  public static method Main(args: Array<string>): int "
        "    return args.Count; "
        "  end Main; "
        "end C;",
        /* args= */ {});
    // No user args → Count == 0 → exit code 0.
    BEZEL_EXPECT_EQ(res.exitCode, 0);
}

// ---------------------------------------------------------------------------
// §33.4: args.Count == N when N user arguments are supplied
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_main_args_633, count_equals_n_args_spec_33_4) {
    auto res = compileAndRun(
        "module M; class C "
        "  public static method Main(args: Array<string>): int "
        "    return args.Count; "
        "  end Main; "
        "end C;",
        /* args= */ {"alpha", "beta", "gamma"});
    // 3 user args → Count == 3 → exit code 3.
    BEZEL_EXPECT_EQ(res.exitCode, 3);
}

// ---------------------------------------------------------------------------
// §33.4: args[0] is the first USER argument (not the executable path)
// ---------------------------------------------------------------------------
// Verify the actual content of args[0] is the first supplied string.
// Use $-interpolation (§18.5) to echo args[0] to stdout and capture it.
BEZEL_TEST(codegen_main_args_633, args_content_first_arg_spec_33_4) {
    auto res = compileAndRun(
        "module M; class C "
        "  public static method Main(args: Array<string>) "
        "    if args.Count > 0 then "
        "      Console.WriteLine($\"{args[0]}\"); "
        "    end; "
        "  end Main; "
        "end C;",
        /* args= */ {"hello"});
    BEZEL_EXPECT_EQ(res.out, std::string("hello\n"));
}

// ---------------------------------------------------------------------------
// §33.6: the canonical hello-world example from the spec
// ---------------------------------------------------------------------------
// "If args.Count > 0 print Hello, {args[0]}!; else print Hello, Bezel!"
BEZEL_TEST(codegen_main_args_633, spec_33_6_no_args_prints_hello_bezel) {
    auto res = compileAndRun(
        "module HelloWorld; "
        "class Program "
        "  public static method Main(args: Array<string>) "
        "    if args.Count > 0 then "
        "      Console.WriteLine($\"Hello, {args[0]}!\"); "
        "    else "
        "      Console.WriteLine(\"Hello, Bezel!\"); "
        "    end; "
        "  end Main; "
        "end Program;",
        /* args= */ {});
    BEZEL_EXPECT_EQ(res.out, std::string("Hello, Bezel!\n"));
}

BEZEL_TEST(codegen_main_args_633, spec_33_6_with_arg_prints_hello_name) {
    auto res = compileAndRun(
        "module HelloWorld; "
        "class Program "
        "  public static method Main(args: Array<string>) "
        "    if args.Count > 0 then "
        "      Console.WriteLine($\"Hello, {args[0]}!\"); "
        "    else "
        "      Console.WriteLine(\"Hello, Bezel!\"); "
        "    end; "
        "  end Main; "
        "end Program;",
        /* args= */ {"World"});
    BEZEL_EXPECT_EQ(res.out, std::string("Hello, World!\n"));
}

// ---------------------------------------------------------------------------
// §33.2 regression: Main() (no-args form) still works after the fix
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_main_args_633, no_args_main_regression_spec_33_2) {
    // The no-args form must not be broken by the fix.
    auto res = compileAndRun(
        "module M; class C "
        "  public static method Main(): int => 42; "
        "end C;",
        /* args= */ {});
    BEZEL_EXPECT_EQ(res.exitCode, 42);
}
