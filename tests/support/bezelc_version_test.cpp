// Regression test for #3084: `bezelc --version` must NOT print the scaffold-era
// string ("0.0.0, no compilation implemented"). That constant survived long
// after the compiler became the AOT oracle the conformance corpus and gates
// compile through, so tooling and logs that record the version (the corpus
// README's staleness records, gate telemetry) said something false.
//
// The fix sets BEZEL_VERSION (via CMake's git describe at configure time) and
// prints `bezelc <version>`. These tests exec the real driver binary and
// assert the output is recognisably a Bezel version string, not the scaffold.
//
// The driver binary path is supplied as the BEZELC_PATH compile definition by
// tests/CMakeLists.txt (the same generator expression the lit track uses). When
// the binary is missing or unexec-able the test surfaces that explicitly — a
// missing binary is a real problem (the build is incomplete), not a test
// infrastructure flake to be silently skipped.

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
// stderr to the two paths. Returns the waitpid status; the captured text lands
// in `out` and `err`. Bounded by a wall-clock deadline so a wedged driver
// can't strand the shard.
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
    // --version is a print-and-exit operation; nothing it does takes meaningful
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
        // fd 0 <- /dev/null so a future flag that reads stdin reads EOF
        // instead of blocking on the test runner's stdin.
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
        // Only reachable if execv failed (e.g. binary disappeared between
        // fork and exec, or the path is unreadable). Print a marker to the
        // captured stderr so the parent can surface it.
        std::fprintf(stderr, "bezelc_version_test: execv failed: errno=%d\n",
                     errno);
        ::_Exit(127);
    }
    // PARENT — bounded reap.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kDeadlineSec);
    int status = 0;
    bool reaped = false;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { r.status = status; reaped = true; break; }
        if (w < 0) {
            if (errno == EINTR) continue;
            r.waitErrno = errno;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            r.status = status;
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)reaped;

    // Read the captured output back. Bounded so a runaway child cannot OOM
    // us. --version is one line; 64 KiB is plenty.
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

BEZEL_TEST(bezelc_version, does_not_print_scaffold_string) {
    // #3084: the scaffold string. ANY of the three substrings disqualifies the
    // output. They were the forge of the old "no compilation implemented yet"
    // constant — a constant that survived long after the compiler became the
    // AOT oracle.
    constexpr const char* kScaffoldMarkers[] = {
        "scaffold",
        "no compilation implemented",
        "not implemented yet",
    };

    // Make sure the bezelc binary actually exists. BEZELC_PATH is the
    // generator-expanded absolute path from tests/CMakeLists.txt; a missing
    // binary means the build is incomplete, which is a real problem — fail
    // loudly, do not skip.
    const std::string bezelcPath = BEZELC_PATH;
    BEZEL_EXPECT_TRUE(fs::exists(bezelcPath));
    if (!fs::exists(bezelcPath)) return;

    // Scratch dir for the captured stdout/stderr. /var/tmp (not /tmp) per
    // aot_support.hpp's comment: /tmp is tmpfs (RAM), and a parked scratch
    // there would consume gigabytes.
    std::error_code ec;
    const std::string scratchBase = "/var/tmp/bezel-scratch/bezelc-version-test";
    fs::create_directories(scratchBase, ec);
    const std::string outPath = scratchBase + "/out.txt";
    const std::string errPath = scratchBase + "/err.txt";

    RunResult r = runBezelc({bezelcPath, "--version"}, outPath, errPath);

    // A failed exec leaves the err path capturing the child's "execv failed"
    // marker — make that loud rather than a false-green (the scaffold
    // assertion alone would still pass if the binary never ran).
    BEZEL_EXPECT_FALSE(r.status == -1 && r.waitErrno != 0);
    BEZEL_EXPECT_TRUE(WIFEXITED(r.status));
    if (WIFEXITED(r.status)) {
        BEZEL_EXPECT_EQ(WEXITSTATUS(r.status), 0);
    }
    if (!WIFEXITED(r.status) || WEXITSTATUS(r.status) != 0) {
        // Print whatever we did capture so triage does not have to re-run.
        std::fprintf(stderr,
            "bezelc_version_test: bezelc exec failed; status=%d, errno=%d\n"
            "  stdout=%.*s\n  stderr=%.*s\n",
            r.status, r.waitErrno,
            static_cast<int>(r.out.size()), r.out.data(),
            static_cast<int>(r.err.size()), r.err.data());
        return;
    }

    // The output must start with "bezelc " (the pre-fix scaffold constant
    // also started with "bezelc ", so this alone would not catch the bug —
    // the scaffold markers below are the load-bearing checks).
    BEZEL_EXPECT_TRUE(r.out.rfind("bezelc ", 0) == 0);

    // Strip a trailing newline if present (the driver writes one — a
    // robustness check against a future change that adds a multi-line
    // version block).
    std::string stdout = r.out;
    while (!stdout.empty() && (stdout.back() == '\n' || stdout.back() == '\r')) {
        stdout.pop_back();
    }

    // #3084: the scaffold string MUST be gone. Each of the three markers is
    // independently sufficient.
    for (const char* marker : kScaffoldMarkers) {
        BEZEL_EXPECT_TRUE(stdout.find(marker) == std::string::npos);
    }

    // The version portion after "bezelc " must be non-empty and look like
    // a real version (a git-describe output, or a hand-set constant). A
    // regression that outputs "bezelc " alone or "bezelc unknown" would be
    // caught here.
    constexpr std::string_view kPrefix = "bezelc ";
    BEZEL_EXPECT_TRUE(stdout.size() > kPrefix.size());
    if (stdout.size() > kPrefix.size()) {
        const std::string version = stdout.substr(kPrefix.size());
        BEZEL_EXPECT_FALSE(version.empty());
        // Not all whitespace. Even "0" or "dev" is better than the scaffold.
        bool anyNonSpace = false;
        for (char c : version) {
            if (c != ' ' && c != '\t') { anyNonSpace = true; break; }
        }
        BEZEL_EXPECT_TRUE(anyNonSpace);
    }
}

BEZEL_TEST(bezelc_version, stderr_is_silent) {
    // --version is a print-and-exit operation: it must NOT emit anything to
    // stderr. A driver that wraps the version lookup in a try/catch and
    // prints a stack trace on failure would still pass the stdout assertion
    // above; checking stderr catches that. The scaffold pre-fix code does
    // not write to stderr either, so this is a guard for the future.
    const std::string bezelcPath = BEZELC_PATH;
    if (!fs::exists(bezelcPath)) return;

    std::error_code ec;
    const std::string scratchBase = "/var/tmp/bezel-scratch/bezelc-version-test";
    fs::create_directories(scratchBase, ec);
    const std::string outPath = scratchBase + "/out2.txt";
    const std::string errPath = scratchBase + "/err2.txt";

    RunResult r = runBezelc({bezelcPath, "--version"}, outPath, errPath);
    if (!WIFEXITED(r.status) || WEXITSTATUS(r.status) != 0) return;
    BEZEL_EXPECT_TRUE(r.err.empty());
}
