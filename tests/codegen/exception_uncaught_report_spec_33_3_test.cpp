// Codegen — the §33.3 uncaught-exception report (v0.163) and #1366.
//
// §33.3: "an exception escaping Main (or the awaited Main task faulting)
// produces exit code 70 (EX_SOFTWARE), AFTER the runtime prints the exception's
// type, message, stack trace, and CauseChain (§31.4) to standard error."
//
// The exit-code half is pinned by uncaught_exit_test.cpp. This file pins the
// REPORT half: the type name and message (and, when present, the flattened
// CauseChain) reach stderr. The exact format string is not normative, so these
// tests assert on the presence of the type name and message substrings, not on
// punctuation.
//
// Uses the native compile+run harness (not the in-process JIT): the uncaught
// path calls exit(70), which would terminate the test process under the JIT.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirUncrep() {
    return wtScratchDir("wt-uncrep");
}

struct RunOutcome {
    int exitCode = -1;
    std::string stderrText;
};

// Compile `source` to a temp native executable via the real Driver pipeline,
// run it with stderr redirected to a temp file, and return (exit code, stderr).
RunOutcome compileAndRunCapturingStderr(const std::string& source) {
    auto dir = wtDirUncrep();
    std::string stem = unique_stem("bezel_uncrep");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    std::string errf = (dir / (stem + ".stderr")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    RunOutcome outcome;
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    if (r.success) {
        std::string cmd = exe + " 2> " + errf;
        int status = std::system(cmd.c_str());
        if (status != -1 && WIFEXITED(status)) outcome.exitCode = WEXITSTATUS(status);
        std::ifstream in(errf);
        outcome.stderrText.assign(std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>());
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    std::remove(errf.c_str());
    return outcome;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// §33.3: an uncaught exception prints its TYPE and MESSAGE to stderr, exit 70.
BEZEL_TEST(codegen_uncaught_report, type_and_message_on_stderr_spec_33_3) {
    auto o = compileAndRunCapturingStderr(
        "module M; class C public static method Main(): int "
        "  throw new InvalidOperationException(\"widget exploded\"); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(o.exitCode, 70);
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "InvalidOperationException"));
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "widget exploded"));
}

// §33.3: a bare (message-less) uncaught exception still reports its type, exit 70.
BEZEL_TEST(codegen_uncaught_report, type_only_no_message_spec_33_3) {
    auto o = compileAndRunCapturingStderr(
        "module M; class C public static method Main(): int "
        "  throw new NotImplementedException(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(o.exitCode, 70);
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "NotImplementedException"));
}

// §33.3 / #811: the CauseChain is rendered too — the cause's type/message reach
// stderr alongside the outer exception's.
BEZEL_TEST(codegen_uncaught_report, cause_chain_on_stderr_spec_33_3) {
    auto o = compileAndRunCapturingStderr(
        "module M; class C public static method Main(): int "
        "  throw new Exception(\"outer failure\", "
        "                      new ArgumentException(\"root cause\", \"amount\")); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(o.exitCode, 70);
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "outer failure"));
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "ArgumentException"));
    BEZEL_EXPECT_TRUE(contains(o.stderrText, "root cause"));
}

// A CAUGHT exception must NOT print the uncaught report (control: the report is
// scoped to the no-handler boundary). Normal completion, exit 0, empty stderr.
BEZEL_TEST(codegen_uncaught_report, caught_exception_no_report_spec_33_3) {
    auto o = compileAndRunCapturingStderr(
        "module M; class C public static method Main(): int "
        "  try throw new Exception(\"handled\"); "
        "  catch Exception => return 0; end; "
        "  return 5; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_TRUE(!contains(o.stderrText, "handled"));
}
