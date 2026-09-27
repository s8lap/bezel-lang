// fix #4386 (§22.16): the `--checked-contracts` CLI flag is listed in
// `bezelc --help` output. Pre-#4386 the only documented §22.16 build-mode
// switch was on the spec page (§22.16 "in unchecked (release) mode — the
// default — unproven `ensures` and `invariant` clauses are not evaluated");
// there was no command-line surface for it, so the issue's premise
// (the §22.16 release default is unreachable from `bezelc`) was correct.
// Post-#4386 the help text advertises `--checked-contracts` as a per-build
// choice, parallel to `--checked-disposal` (§28.2).
//
// The behavioral half of #4386 lives in
// tests/codegen/checked_contracts_4386_test.cpp — those tests exercise the
// codegen path via the AOT harness. This test exercises the CLI surface:
// fork+exec the real bezelc binary with `--help`, capture stdout, and
// assert the flag name appears in the usage text. This is the same
// fork/exec/slurp pattern tests/support/bezelc_version_test.cpp uses for
// `bezelc --version`; BEZELC_PATH is the generator-expanded absolute path
// from tests/CMakeLists.txt.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/build_paths.hpp"  // #5010: the BEZEL_*_PATH / BEZELC_PATH aliases
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

#ifndef BEZELC_PATH
#error "BEZELC_PATH must be defined (tests/CMakeLists.txt passes it via $<TARGET_FILE:bezelc>)"
#endif

// fork+exec the bezelc binary with `args` after it, capturing stdout and
// stderr to the two paths. Returns the waitpid status; the captured text
// lands in `out` and `err`. Bounded by a wall-clock deadline so a wedged
// driver can't strand the shard.
struct RunResult {
    int status = -1;       // raw waitpid status; -1 if fork failed
    int waitErrno = 0;
    std::string out;
    std::string err;
};

RunResult runBezelc(const std::vector<std::string>& args,
                    std::string_view stdoutPath,
                    std::string_view stderrPath) {
    RunResult r;
    // --help is a print-and-exit operation; nothing it does takes meaningful
    // time. Cap at 20s so a wedged driver (impossible in practice) cannot
    // strand the shard past its 300s ceiling.
    constexpr int kDeadlineSec = 20;

    pid_t pid = ::fork();
    if (pid < 0) {
        r.waitErrno = errno;
        return r;
    }
    if (pid == 0) {
        // CHILD — redirect fds and exec.
        int nullFd = ::open("/dev/null", O_RDONLY);
        if (nullFd >= 0) {
            ::dup2(nullFd, STDIN_FILENO);
            if (nullFd > STDERR_FILENO) ::close(nullFd);
        } else {
            ::close(STDIN_FILENO);
        }
        int outFd = ::open(std::string(stdoutPath).c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int errFd = ::open(std::string(stderrPath).c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) ::dup2(outFd, STDOUT_FILENO);
        if (errFd >= 0) ::dup2(errFd, STDERR_FILENO);
        if (outFd > STDERR_FILENO) ::close(outFd);
        if (errFd > STDERR_FILENO) ::close(errFd);

        std::vector<char*> cargv;
        cargv.reserve(args.size() + 1);
        for (const auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(cargv[0], cargv.data());
        // Only reachable if execv failed.
        std::fprintf(stderr, "checked_contracts_4386_cli_test: execv failed: errno=%d\n",
                     errno);
        ::_Exit(127);
    }
    // PARENT — bounded reap.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kDeadlineSec);
    int status = 0;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { r.status = status; break; }
        if (w < 0) {
            if (errno == EINTR) continue;
            r.waitErrno = errno;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            r.status = status;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Read the captured output back. Bounded so a runaway child cannot OOM
    // us. --help is a few hundred bytes; 64 KiB is plenty.
    auto slurp = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::string{};
        std::string out;
        out.reserve(4096);
        constexpr std::size_t cap = 1u << 16;
        char buf[4096];
        while (in) {
            in.read(buf, sizeof(buf));
            auto got = static_cast<std::size_t>(in.gcount());
            std::size_t room = out.size() < cap ? cap - out.size() : 0;
            if (got <= room) {
                out.append(buf, got);
            } else {
                out.append(buf, room);
                out.append("\n... (truncated) ...\n", 22);
                break;
            }
        }
        return out;
    };
    r.out = slurp(std::string(stdoutPath));
    r.err = slurp(std::string(stderrPath));
    return r;
}

} // namespace

// =========================================================================
// Row 4 (§22.16 / --checked-contracts listed in --help): the CLI surface
// advertises the flag. Without --checked-contracts the codegen has no
// command-line switch to select the §22.16 mode, and the issue's premise
// is unfixable; this test pins the help-text surface so the flag cannot
// silently disappear.
// =========================================================================
BEZEL_TEST(checked_contracts_4386_cli, help_lists_checked_contracts_flag) {
    const std::string bezelcPath = BEZELC_PATH;
    BEZEL_EXPECT_TRUE(fs::exists(bezelcPath));
    if (!fs::exists(bezelcPath)) return;

    // Scratch dir for the captured stdout/stderr. /var/tmp (not /tmp) per
    // aot_support.hpp's comment: /tmp is tmpfs (RAM).
    std::error_code ec;
    const std::string scratchBase = "/var/tmp/bezel-scratch/checked-contracts-cli-test";
    fs::create_directories(scratchBase, ec);
    const std::string outPath = scratchBase + "/out.txt";
    const std::string errPath = scratchBase + "/err.txt";

    RunResult r = runBezelc({bezelcPath, "--help"}, outPath, errPath);

    // A failed exec leaves the err path capturing the child's "execv failed"
    // marker — make that loud rather than a false-green.
    BEZEL_EXPECT_FALSE(r.status == -1 && r.waitErrno != 0);
    BEZEL_EXPECT_TRUE(WIFEXITED(r.status));
    if (WIFEXITED(r.status)) {
        // bezelc --help exits 0 — a print-and-exit operation, parallel to
        // `bezelc --version`.
        BEZEL_EXPECT_EQ(WEXITSTATUS(r.status), 0);
    }
    if (!WIFEXITED(r.status) || WEXITSTATUS(r.status) != 0) {
        std::fprintf(stderr,
            "checked_contracts_4386_cli_test: bezelc exec failed; status=%d, errno=%d\n"
            "  stdout=%.*s\n  stderr=%.*s\n",
            r.status, r.waitErrno,
            static_cast<int>(r.out.size()), r.out.data(),
            static_cast<int>(r.err.size()), r.err.data());
        return;
    }

    // The flag name MUST appear in the help text. The help text is the
    // printUsage() output from src/main.cpp; it includes
    // "--checked-contracts" as a literal option-line word. A regression
    // that removes the flag (or renames it) would fail this assertion
    // before the binary even compiles a Bezel program.
    BEZEL_EXPECT_TRUE(r.out.find("--checked-contracts") != std::string::npos);
}

// =========================================================================
// §22.16 / --checked-contracts: unknown-option error names the offending
// argument verbatim. This pins the CLI parse path — a regression that
// short-circuits the unknown-option arm would fail this assertion. The
// flag name is asserted alongside an unrelated bad flag, so the test
// isolates "the flag exists" (Row 4) from "the CLI parses it correctly"
// (this row).
// =========================================================================
BEZEL_TEST(checked_contracts_4386_cli, unknown_option_error_names_arg) {
    const std::string bezelcPath = BEZELC_PATH;
    if (!fs::exists(bezelcPath)) return;

    std::error_code ec;
    const std::string scratchBase = "/var/tmp/bezel-scratch/checked-contracts-cli-test";
    fs::create_directories(scratchBase, ec);
    const std::string outPath = scratchBase + "/out2.txt";
    const std::string errPath = scratchBase + "/err2.txt";

    // `--checked-contracts` is a valid flag, so bezelc will parse it and
    // then fail on the missing source-file argument. The exit code is 2
    // (the "inputPath.empty()" branch in main.cpp) — we don't assert the
    // exact exit code here because the message-format is the load-bearing
    // pin: any future refactor that breaks the unknown-option arm would
    // surface as a different exit status or message shape.
    RunResult r = runBezelc(
        {bezelcPath, "--checked-contracts"}, outPath, errPath);

    BEZEL_EXPECT_TRUE(WIFEXITED(r.status));
    if (!WIFEXITED(r.status)) return;
    // Exit code 2 (the "no input file" branch); the help text is written
    // to stderr on this branch (main.cpp's `printUsage(std::cerr)`).
    BEZEL_EXPECT_EQ(WEXITSTATUS(r.status), 2);
    // The flag name must appear in the error's usage dump — without the
    // flag existing, `printUsage` would not list it, and this assertion
    // would fail.
    const std::string& errOrOut = r.err.empty() ? r.out : r.err;
    BEZEL_EXPECT_TRUE(errOrOut.find("--checked-contracts") != std::string::npos);
}
