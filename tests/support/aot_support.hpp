#pragma once

// Canonical AOT test-harness helper — the JIT->AOT test-migration foundation
// (migration plan §2, P0). `runAot`/`runAotChecked` are drop-in counterparts to
// the in-process JIT `runJit`/`runJitChecked` (support/codegen_support.hpp):
// instead of ORC-JITting `main` in the test process, they compile the source to
// a NATIVE executable and run it in a subprocess — the same path `bezelc` ships,
// and therefore the stricter, correct oracle (it runs ownership + definite-
// assignment + static-init-cycle + §33.5 entry-point checks the JIT harness
// deliberately skips).
//
// CONSOLIDATION, NOT GREEN FIELD. Both helpers drive the REAL driver
// (`bezel::Driver::compileFile`) — the shipping bezelc AOT path (full front-end
// -> emitObjectFile -> clang link) — so the link recipe is NOT re-written here.
// They differ by exactly ONE bit: `CodegenOptions::sanitizeAddress`.
//
// FORKED IN-PROCESS COMPILE (quality round 1, qr1 M1). The compile runs
// `Driver::compileFile` IN-PROCESS but inside a fork()ed child, mirroring the
// proven `runJit` isolation (codegen_support.hpp jit_isolate):
//   * CRASH ISOLATION (M1). The front-end has a live crash history on error-
//     recovery ASTs (#1622/#1615); negative tests feed it malformed programs. A
//     front-end assert/segfault now dies in the forked CHILD -> the parent test
//     binary survives (ran=false, isolated), instead of aborting the whole gtest
//     shard and losing every other test in it. This is the exact hazard runJit
//     forks to avoid. In-process (unforked) it was fatal.
//   * COMPILE TIMEOUT (M1). The parent waitpids the compile child under a
//     deadline and SIGKILLs on expiry, so a front-end infinite loop or a hung
//     clang link can't wedge the shard to its 300s cap (the unforked path had no
//     compile deadline at all).
//   * SPEED. The forked in-process compile is ~2.11x a JIT test (P0-E shadow
//     run) — the fork cost is negligible against the clang link already spent.
//     Shelling out to the `bezelc` binary was measured ~11x (P0-E), ~5x slower,
//     so the binary path was rejected: fork gives the SAME isolation far cheaper.
//
// ONE PIPELINE, NO DRIFT (S2). runAot and runAotChecked both call the SAME
// `Driver::compileFile` (via the shared forkedCompile below), so they run the
// identical front-end — INCLUDING the §33.5 entry-point check compileFile runs
// on the executable path. They differ only by `opts.sanitizeAddress`; a program
// runAot rejects, runAotChecked rejects identically. (The pre-qr1 runAotChecked
// re-implemented the pipeline inline and had drifted, skipping §33.5.)
//
// GENERATED-CODE ASAN (S3). runAotChecked sets `opts.sanitizeAddress = true`, so
// Driver::compileFile runs LLVM's AddressSanitizerPass over the GENERATED module
// (emitObjectFile, LlvmBackend.cpp) and links the executable -fsanitize=address.
// A use-after-free / out-of-bounds READ performed by an emitted Bezel load is
// then caught at the access — not merely the allocator-level leaks/double-frees
// an instrumented runtime alone nets. This is the in-process equivalent of
// `bezelc --sanitize=address`, closing the WS2 gap. (LLVM 21 pass + system
// clang 21 ASan runtime => matching ABI; verified E6.)
//
// QUALITY ROUND 2 (qr2) — three hardening items layered on round 1:
//   * RUN-TIMEOUT TUNING (S4). The compiled-PROGRAM deadline defaults to 30s,
//     deliberately LOWER than runJit's 60s: an AOT test program is a tiny native
//     executable that runs in well under a millisecond, so the only thing the
//     deadline bounds is a genuine hang. A tighter bound lets the shared 300s
//     shard budget (run_parallel.sh) absorb more hangs before the shard is lost.
//     The COMPILE deadline stays SEPARATE at 120s (linking legitimately takes
//     seconds). BEZEL_AOT_TIMEOUT_SEC overrides.
//   * STALE-WORKDIR REAPER (N1). WorkDir RAII cleans the mkdtemp dir on every
//     normal path — including the compile/run child-timeout paths, where the
//     PARENT survives the child SIGKILL and its dtor still runs. The leak this
//     closes is the WHOLE PROCESS being killed (the 300s shard cap SIGKILL, or an
//     OOM kill): the dtor never runs and the dir leaks. The first makeWorkDir()
//     per process reaps aot* dirs older than 30 min — an age gate that never
//     races a concurrently-running sibling shard's in-flight dir.
//   * A COMMITTED crash-isolation regression test pins M1 (tests/support/
//     aot_crash_isolation_test.cpp) via the BEZEL_AOT_TEST_CRASH_HOOK affordance.
//
// RETURN TYPE: both return `JitResult` (the runJit struct), so every downstream
// `.ran` / `.exitCode` / `.diagnostics` call site is untouched (migration plan
// §2; WS6 §3). The added `JitResult::stdoutText` carries the captured program
// stdout — the value channel for observables that cannot fit an 8-bit exit code.
// On a compile/link REJECT (or compiler crash/timeout), the diagnostics carry
// one Error whose message is the captured driver stderr text (S1: no test
// inspects a JitResult diagnostic's .location; the stderr blob keeps
// .hasErrors() true and message.find(...) assertions matching).
//
// INFRASTRUCTURE FAILURES ARE LOUD, AND DISTINGUISHABLE (#3007). A workdir
// create, a source write, a fork(), or an execv() that fails is NOT the
// compiler's verdict on the program — the program never ran. Before #3007 each
// of those returned `ran=false` + one Severity::Error carrying the message,
// which is BYTE-FOR-BYTE the shape of a legitimate compile REJECT: the 272
// `BEZEL_EXPECT_TRUE(<var>.diagnostics.hasErrors())` and 205
// `BEZEL_EXPECT_FALSE(<var>.ran)` assertions in the 1,446 test files that reach
// this harness (measured 2026-09-03; #3007's own 1,008 + 84 = 1,092 does NOT
// reproduce — it counted across ALL of tests/, most of which belongs to the
// in-process JIT/sema harnesses this path cannot reach) would all have gone
// GREEN on a full disk, certifying rules that were never evaluated. (`hasErrors()` is the assertion across most
// of tests/ownership/ — #2988 — so the harness was handing those tests nothing
// stronger to assert on.) Latency was luck, not design: the issue was filed
// while the harness workdir was on a 1.3 TiB spindle, and scripts/lane_setup.sh
// has since symlinked scratchBase() onto /mnt/fast, which HAS hit ENOSPC.
//
// So every non-verdict failure now goes through aot_detail::noteInfraFailure():
//   * it marks the CURRENT TEST FAILED (bezel::test::currentTestFailed()) —
//     unconditionally, before the caller sees a result, so no assertion of any
//     shape can read an infra failure as green;
//   * it prints a `[ INFRA ]` marker naming the stage and the errno;
//   * it counts into a process-wide tally printed at exit as
//     `INFRA: N failure(s) ...` (the sibling of test_main.cpp's
//     `NATIVE SANITIZER: N check(s) skipped`, #3626), which a later change can
//     sum across shards in tests/run_parallel.sh; and
//   * the returned JitResult carries the reason at Severity::NOTE, never Error,
//     so `hasErrors()` stays FALSE and a legacy negative assertion FAILS rather
//     than passing for the wrong reason.
// The self-tests are tests/support/aot_infra_loud_3007_test.cpp.
//
// TWO SEMANTIC DIFFERENCES FROM runJit (documented, inherent — not bugs here):
//   1. runJit returns the raw 32-bit int from main(); a process exit status is
//      8-bit (WEXITSTATUS masks to 0..255). A test whose Main returns a value
//      outside 0..255 (256, 1559, -1, ...) does NOT round-trip through the exit
//      code and must move its observable to stdout (captured in stdoutText).
//   2. The AOT front-end is STRICTER: a program runJit accepts (it skips the
//      ownership / DA / static-init / §33.5 entry-point passes) may be REJECTED
//      here, surfacing as ran=false with the rejection in `diagnostics`.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.hpp"              // currentTestFailed() — the #3007 trip
#include "support/codegen_support.hpp"   // JitResult + codegen::CodegenOptions
#include "support/force_sanitize.hpp"    // the ONE BEZEL_AOT_FORCE_SANITIZE parse
#include "support/scratch_dir.hpp"      // cacheDir/scratchBase/keepArtifacts/reapAgeSec
#include "driver/Driver.hpp"             // Driver::compileFile (the AOT path)

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"         // content-addressed link-cache key

namespace bezel::test {

namespace aot_detail {

namespace fs = std::filesystem;


// Per-test wall-clock deadline for the compiled PROGRAM run (seconds) — a hung
// program cannot wedge the run. qr1 S4: the default is 30s, deliberately LOWER
// than runJit's 60s. An AOT test program is a tiny native executable that runs
// in well under a millisecond; the deadline exists ONLY to bound a genuine hang
// (infinite loop / deadlock). The shard budget is a hard ~300s cap
// (run_parallel.sh BEZEL_SHARD_TIMEOUT) shared by every test in the shard, and
// AOT is heavier per test (real clang link + native exec), so a shard absorbs
// far fewer hangs than the JIT suite — at 60s each, ~4 hung programs exhaust the
// budget and lose the whole shard; at 30s the same budget tolerates twice as
// many, while still leaving 30s of slack for any legitimately slow program
// (there are none at this scale). The COMPILE deadline is SEPARATE
// (aotCompileTimeoutSec, 120s) because linking legitimately takes seconds.
// BEZEL_AOT_TIMEOUT_SEC overrides; 0 disables the deadline.
inline int aotTimeoutSec() {
    static const int v = [] {
        const char* e = std::getenv("BEZEL_AOT_TIMEOUT_SEC");
        if (!e || *e == '\0') return 30;
        int n = std::atoi(e);
        return n >= 0 ? n : 30;
    }();
    return v;
}

// Wall-clock deadline for the forked COMPILE+LINK (seconds). qr1 M1: the
// unforked compile had NO deadline — a front-end infinite loop or a hung `clang`
// link would wedge the shard to its 300s cap. Linking legitimately takes a few
// seconds cold, so the default is generous (120s): it only bounds a genuine
// hang, not a slow-but-progressing compile. BEZEL_AOT_COMPILE_TIMEOUT_SEC
// overrides; 0 disables.
inline int aotCompileTimeoutSec() {
    static const int v = [] {
        const char* e = std::getenv("BEZEL_AOT_COMPILE_TIMEOUT_SEC");
        if (!e || *e == '\0') return 120;
        int n = std::atoi(e);
        return n >= 0 ? n : 120;
    }();
    return v;
}

// Default cap on captured-output reads: 256 KiB, matching runJit's readStderr
// (codegen_support.hpp). qr1 M2: a runaway program that prints unbounded to
// stdout must NOT be pulled wholesale into RAM and OOM the shard under the
// 8-12G dev_heavy cage.
inline constexpr std::size_t kSlurpCap = 1u << 18;  // 256 KiB

// Read up to `cap` bytes of `path` into a string (qr1 M2 — bounded so a
// runaway/adversarial program can't OOM the shard). If the file is longer than
// `cap`, the excess is dropped and a truncation marker is appended.
inline std::string slurp(const std::string& path, std::size_t cap = kSlurpCap) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string out;
    out.reserve(std::min<std::size_t>(cap, 1u << 16));
    char buf[8192];
    bool truncated = false;
    while (in) {
        in.read(buf, sizeof(buf));
        auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        std::size_t room = out.size() < cap ? cap - out.size() : 0;
        if (got <= room) {
            out.append(buf, got);
        } else {
            out.append(buf, room);
            truncated = true;
            break;
        }
    }
    if (truncated) out += "\n[...output truncated at 256 KiB...]";
    return out;
}

// waitpid `pid` under a wall-clock deadline. On timeout the child is SIGKILLed
// and reaped; *timedOut is set. timeoutSec <= 0 blocks with no deadline. Returns
// the raw waitpid status.
inline int reapWithDeadline(pid_t pid, int timeoutSec, bool& timedOut) {
    timedOut = false;
    int status = 0;
    if (timeoutSec <= 0) {
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        return status;
    }
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) return status;                       // reaped
        if (w < 0) { if (errno == EINTR) continue; break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status;
}

// fork/exec `argv` with fd 0 <- /dev/null, fd 1 -> outPath and fd 2 -> errPath
// (both truncated), applying `extraEnv` (setenv in the child before exec — used
// for ASAN_OPTIONS). Blocks until the child exits or the deadline elapses; on
// timeout the child is SIGKILLed and reaped. Returns the raw waitpid status and
// sets *timedOut. timeoutSec <= 0 disables the deadline. Returns -1 if fork()
// itself fails.
//
// qr1 M4: stdin is redirected to /dev/null so a program that reads Console input
// gets immediate EOF instead of blocking to the deadline (or draining the test
// runner's own stdin).
//
// fix #3007: `execErrno` (optional, defaulted so the three external call sites
// are untouched) reports an execv() FAILURE distinguishably. Before this, a
// failed exec left the child at `_Exit(127)`, which the caller read as
// `ran=true, exitCode=127` — indistinguishable from a Bezel program whose Main
// legitimately returns 127, and therefore a silent wrong answer rather than the
// infrastructure failure it is. The classic close-on-exec errno pipe settles it
// with no guesswork: the write end has O_CLOEXEC, so a SUCCESSFUL exec closes
// it and the parent reads EOF; a FAILED exec writes errno through it first.
//
// pipe2(O_CLOEXEC), NOT pipe() + fcntl(FD_CLOEXEC) (review nit N1). The two-call
// form has a window between the pipe and the fcntl in which a CONCURRENT
// fork+exec would inherit the write end WITHOUT the close-on-exec flag. That
// inherited copy keeps the pipe's write side open after our own exec succeeds,
// so the parent's read() below never sees EOF and BLOCKS FOREVER — a wedged
// shard, the worst failure shape a test harness can have, and one that would
// look like a compiler hang. The harness is single-threaded today so the window
// is unreachable, but the atomic form costs nothing and removes the reasoning
// step entirely.
inline int spawnCaptured(const std::vector<std::string>& argv,
                         const std::string& outPath, const std::string& errPath,
                         int timeoutSec, bool& timedOut,
                         const std::vector<std::pair<std::string, std::string>>&
                             extraEnv = {},
                         int* execErrno = nullptr) {
    timedOut = false;
    if (execErrno) *execErrno = 0;
    // Close-on-exec errno pipe (#3007), created ATOMICALLY (nit N1 — see the
    // function comment for the shard-hang the two-call form could produce).
    // Best-effort: if pipe2() fails we lose the discrimination, never the run.
    int execPipe[2] = {-1, -1};
    if (::pipe2(execPipe, O_CLOEXEC) != 0) execPipe[0] = execPipe[1] = -1;
    pid_t pid = ::fork();
    if (pid < 0) {
        // Preserve the fork() errno across the pipe cleanup: the caller reads
        // errno to build the `[ INFRA ]` reason (#3007), and close() would
        // otherwise be free to clobber it.
        const int forkErrno = errno;
        if (execPipe[0] >= 0) { ::close(execPipe[0]); ::close(execPipe[1]); }
        errno = forkErrno;
        return -1;
    }
    if (pid == 0) {
        // ---- CHILD ----
        for (const auto& kv : extraEnv)
            ::setenv(kv.first.c_str(), kv.second.c_str(), /*overwrite=*/1);
        // qr1 M4: fd 0 <- /dev/null so a stdin read gets EOF, never blocks.
        int nullFd = ::open("/dev/null", O_RDONLY);
        if (nullFd >= 0) {
            ::dup2(nullFd, STDIN_FILENO);
            if (nullFd > STDERR_FILENO) ::close(nullFd);
        } else {
            ::close(STDIN_FILENO);
        }
        int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int errFd = errPath == outPath
                        ? outFd
                        : ::open(errPath.c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) ::dup2(outFd, STDOUT_FILENO);
        if (errFd >= 0) ::dup2(errFd, STDERR_FILENO);
        // qr2 audit: close the ORIGINAL descriptors — they survive as fd 1/2 via
        // dup2, and leaving them open would leak stray fds onto out/err into the
        // exec'd program (which could observe or write them). Guard the
        // errFd==outFd alias (same fd when errPath==outPath).
        if (errFd > STDERR_FILENO && errFd != outFd) ::close(errFd);
        if (outFd > STDERR_FILENO) ::close(outFd);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        if (execPipe[0] >= 0) ::close(execPipe[0]);   // child holds only the write end
        // Fix narrow #3007 retry: ETXTBSY (errno 26, "Text file busy") is a
        // TRANSIENT FS-level race, not a real exec failure. It fires when
        // another process / thread holds the executable open for writing —
        // the async_frame_teardown_no_uaf_3460 reproducer measures ~1-3 %
        // pre-fix failure on this exact symptom: it forks 8 threads × K=32
        // attempts each, all hammering the SAME cached executable inode
        // (one fs::create_hard_link per attempt), and a concurrent sibling
        // shard's clang link (forkedLink's child) or storeToCache rename
        // can leave a brief window where the kernel refuses the exec.
        //
        // Bounded retry (≤ kMaxExecRetries attempts with kExecRetrySleepMs
        // between) closes the window. ONLY ETXTBSY is retried — EACCES,
        // ENOENT, ENOMEM, and every other errno are real failures and pass
        // through immediately, so the harness's "[ INFRA ]" channel still
        // surfaces genuine compile/link or harness errors at the same
        // fidelity. The total retry budget is ≤ 25 ms (5 attempts × 5 ms),
        // negligible against aotTimeoutSec (30 s). The execPipe[1] write end
        // stays open across retries (it's O_CLOEXEC — a successful exec
        // would still close it automatically); we only WRITE the final errno
        // after the retry budget is exhausted, so the parent receives the
        // REAL outcome, not the first transient one.
        int execRetryErrno = 0;
        constexpr int kMaxExecRetries = 5;
        constexpr int kExecRetrySleepMs = 5;
        for (int attempt = 0;; ++attempt) {
            ::execv(cargv[0], cargv.data());
            execRetryErrno = errno;
            if (execRetryErrno != ETXTBSY || attempt >= kMaxExecRetries) break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kExecRetrySleepMs));
        }
        const int e = execRetryErrno;
        // exec failed — report errno through the CLOEXEC pipe (#3007) so the
        // parent can tell this apart from a program that returned 127 itself.
        if (execPipe[1] >= 0) {
            ssize_t ignored = ::write(execPipe[1], &e, sizeof(e));
            (void)ignored;
        }
        ::_Exit(127);  // exec failed
    }
    // ---- PARENT ----
    if (execPipe[1] >= 0) ::close(execPipe[1]);   // parent holds only the read end
    int status = reapWithDeadline(pid, timeoutSec, timedOut);
    if (execPipe[0] >= 0) {
        // The child has been reaped, so the pipe is already at EOF (exec
        // succeeded) or holds exactly one int (exec failed): a single read
        // cannot block.
        int e = 0;
        const ssize_t n = ::read(execPipe[0], &e, sizeof(e));
        ::close(execPipe[0]);
        if (n == static_cast<ssize_t>(sizeof(e)) && execErrno) *execErrno = e;
    }
    return status;
}


// A per-call working directory under scratchBase(). Empty string on failure.
//
// fix #3007: on failure, `why` (when non-null) receives the CAUSE — the errno
// from mkdtemp plus any create_directories error. The empty return alone told
// the caller nothing, so the diagnostic it built named only the directory and
// read exactly like a compiler reject; "No space left on device" is what makes
// the `[ INFRA ]` marker actionable instead of merely loud.
inline std::string makeWorkDir(std::string* why) {
    if (why) why->clear();
    std::error_code ec;
    fs::create_directories(scratchBase(), ec);
    reapStaleWorkDirs();  // qr1 N1: sweep prior-run leaks once per process.
    std::string tmpl = scratchBase() + "/aotXXXXXX";
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back('\0');
    errno = 0;
    const char* dirp = ::mkdtemp(tbuf.data());
    if (dirp) return std::string(dirp);
    if (why) {
        const int e = errno;
        *why = "could not create a work dir under " + scratchBase() +
               " (mkdtemp: errno=" + std::to_string(e) + ": " +
               std::strerror(e) + ")";
        if (ec) *why += " [create_directories: " + ec.message() + "]";
    }
    return {};
}

// Signature-preserving overload — the three existing aot_detail::makeWorkDir()
// call sites (tests/support/aot_cache_key_test.cpp) are untouched.
inline std::string makeWorkDir() { return makeWorkDir(nullptr); }

// RAII cleanup of a work dir (kept when BEZEL_AOT_KEEP is set, for debugging).
struct WorkDir {
    std::string path;
    ~WorkDir() {
        if (path.empty()) return;
        if (keepArtifacts()) {
            std::fprintf(stderr, "[aot] kept %s\n", path.c_str());
            return;
        }
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// True if `text` carries an ASan / LSan report headline — the runAotChecked
// "sanitizer fired" signal (qr1 M3: the UNAMBIGUOUS signal, independent of any
// exit-code sentinel a program might legitimately return). With
// abort_on_error=0 the sanitizer always PRINTS one of these before exiting, so
// the presence of a report — not the bare exit code — is the reliable oracle.
inline bool hasSanitizerReport(const std::string& text) {
    static const char* markers[] = {
        "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer", "detected memory leaks",
    };
    for (const char* m : markers)
        if (text.find(m) != std::string::npos) return true;
    return false;
}

// #1822: whole-suite ASan sweep switch. When BEZEL_AOT_FORCE_SANITIZE is set to
// "address" / "asan" / "1", EVERY runAot program is compiled and linked with
// generated-code AddressSanitizer (opts.sanitizeAddress) and run under ASan — the
// same proven recipe runAotChecked uses per-test (S3), applied suite-wide so a
// memory error (use-after-free / out-of-bounds / double-free) in ANY test program
// is caught. scripts/asan_sweep.sh sets this; unset/empty (the default) leaves
// normal runs byte-for-byte unchanged. Memoized once per process.
//
// This replaces the pre-#1822 sweep, which rebuilt the C++ HARNESS with
// -fsanitize=address — instrumenting bezel_tests, not the generated Bezel code —
// and under the AOT harness only broke the per-program links (plain-clang links
// against an ASan-instrumented runtime archive → undefined __asan_* refs), so no
// program ran under ASan and the sweep validated NOTHING (false-green).
//
// THE DEFINITION MOVED to support/force_sanitize.hpp and this is now a re-export,
// so the `aot_detail::forceSanitizeAddress()` call sites below are untouched.
// The reason it moved: there were THREE independent copies of this parse — here,
// in test_main.cpp's shard-summary writer, and (implicitly, by absence) in the
// ~200 hand-rolled `Driver::compileFile` runners, which had none at all and were
// therefore invisible to the sweep. One definition, three consumers; the third
// is support/driver_run_support.hpp.
using bezel::test::forceSanitizeAddress;

// #1822: re-emit a fired sanitizer report to the harness's real stderr (fd 2) so
// the whole-suite sweep (scripts/asan_sweep.sh) can grep the shard log for the
// "ERROR: AddressSanitizer" / "ERROR: LeakSanitizer" headline. A clean suite
// never reaches here (a report only exists on a real memory error), so this is
// silent in normal operation. Bounded so an adversarial report cannot flood the
// shard log.
inline void emitSanitizerReportToStderr(std::string_view report) {
    const std::size_t cap = 1u << 15;  // 32 KiB — the headline + backtrace
    std::fprintf(stderr,
        "\n===== [aot-asan #1822] sanitizer fired in an AOT test program =====\n"
        "%.*s\n"
        "===== [aot-asan #1822] end sanitizer report =====\n",
        static_cast<int>(std::min(report.size(), cap)), report.data());
    std::fflush(stderr);
}


// ── INFRASTRUCTURE-FAILURE CHANNEL (fix #3007) ─────────────────────────────
//
// THE DEFECT. A workdir create, a source write, a fork(), an exec(), or a
// driver that reports success and produces no executable is not a verdict on
// the program under test — the program NEVER RAN. Every one of those used to
// come back as `ran=false` plus one `Severity::Error` carrying the message,
// which is exactly the shape of a legitimate compile REJECT. The two negative
// assertions this tree is built on —
//     BEZEL_EXPECT_TRUE(<var>.diagnostics.hasErrors());   (272 sites)
//     BEZEL_EXPECT_FALSE(<var>.ran);                      (205 sites)
// — are both SATISFIED by that shape, so on a full disk ~477 assertions go
// green while certifying nothing. (Counts measured 2026-09-03 over the 1,446
// test files that call a runAot* helper — the only ones this harness reaches.
// #3007 states 1,008 + 84 = 1,092; that figure does not reproduce and appears
// to have counted all 3,015 hasErrors() assertions in tests/, including the
// in-process JIT/sema harnesses. The defect is unchanged in kind, only in size.) `hasErrors()` cannot distinguish "the rule
// under test rejected this" from "anything at all went wrong" (#2988), and the
// harness was giving those tests nothing stronger to assert on.
//
// WHY A HARNESS-LEVEL TRIP AND NOT A `JitResult::infraFailure` FIELD. A field
// is only ever ADVISORY: it makes the failure observable to a call site that
// remembers to look, which is ~477 edits away and re-creates the very
// opt-in-per-site shape that produced the bug (see #3633 — the ~122 per-file
// `runUnderAsan` helpers whose `-1` each caller had to interpret, and did not).
// Marking the CURRENT TEST FAILED inside the harness is unconditional: it needs
// no cooperation from any assertion, so the fix holds for every one of those
// sites the day it lands, and for every site written afterwards. The two are not
// alternatives on a soundness axis — once the harness fails the test, a field
// adds nothing a test could act on — so the field is deliberately NOT added
// (it would also have to live in tests/support/codegen_support.hpp, shared with
// the JIT path, for no gain). The result is still made HONEST: `ran=false`,
// `exitCode=-1`, and the reason recorded at Severity::NOTE, never Error, so
// `hasErrors()` is FALSE and a legacy negative assertion FAILS loudly instead of
// passing for the wrong reason. That is belt (harness trip) and braces (the
// assertion itself can no longer be satisfied).
//
// WHY NOT std::abort(). The harness runs each test as a plain call from
// test_main.cpp's loop, with no try/catch and no per-test process — so an abort
// or a throw takes the WHOLE SHARD, converting one infra failure into the loss
// of every other test in it. Under a genuine ENOSPC that is every shard, which
// destroys precisely the information the fix exists to produce: WHICH tests were
// affected, and how many. A per-test FAIL plus a process-wide tally keeps the
// suite's own accounting intact.

// Process-wide count of infrastructure failures. Leaked on purpose: the atexit
// hook below reads it at teardown, after ordinary function-local statics would
// already have been destroyed (same discipline as forkFailureCount()).
inline std::atomic<std::uint64_t>& infraFailureCount() {
    static std::atomic<std::uint64_t>* c = new std::atomic<std::uint64_t>(0);
    return *c;
}

// Scoped interception, FOR THE HARNESS'S OWN SELF-TESTS ONLY. While one of
// these is alive, noteInfraFailure() records into it and does NOT fail the
// current test; the destructor removes this scope's contribution from the
// process tally, so a synthetic probe cannot poison the `INFRA:` line the run
// reports (the #3626 self-test has to restore its counter by hand — this makes
// the restore structural). Never use it to silence a real infra failure.
//
// ⚠ THIS IS A PROCESS-GLOBAL BYPASS OF THE #3007 TRIP, and the only one that
// exists (review nit N4). The pointer is a plain global, so an InfraExpectation
// that OUTLIVES its test body — parked in a `static`, a namespace-scope object,
// or leaked with `new` — silently disarms the trip for every remaining test in
// the shard, restoring exactly the false green this whole change removes, with
// no marker anywhere. The ONLY sanctioned use is a stack-scoped local inside
// tests/support/aot_infra_loud_3007_test.cpp. Two properties keep that honest
// and both are load-bearing: it is non-copyable, and its destructor restores
// the PREVIOUS pointer rather than clearing to null, so nesting cannot strand
// a stale one. A cheap belt for the future is asserting
// `currentInfraExpectation() == nullptr` after each test body in
// tests/test_main.cpp — not done here because this lane does not own that file.
struct InfraExpectation;
inline InfraExpectation*& currentInfraExpectation() {
    static InfraExpectation* p = nullptr;
    return p;
}

// One-line summary printed at process exit, unconditionally once the AOT
// harness has been used at all — so `INFRA: 0 failure(s)` is a POSITIVE
// confirmation the channel was live this process, not merely the absence of a
// signal (#3626's lesson, applied to its sibling channel). Format, stable and
// greppable, on stdout (shard logs capture stdout and stderr into one file):
//
//     INFRA: <N> failure(s) (test-harness infrastructure: work dir / source
//     write / fork / exec — see tests/support/aot_support.hpp)
//
// A later change can sum it across shards in tests/run_parallel.sh exactly as
// it sums `native_sanitizer_unavailable_skips` today — grep -ho 'INFRA: [0-9]*'
// over shard.*.log — and fail the sharded run on a nonzero total. That wiring
// is deliberately NOT done here: this lane makes the harness honest, and the
// gate floor is a separate, gate-owned change.
inline void ensureInfraTallyAtExit() {
    static const bool once = [] {
        std::atexit([] {
            std::fprintf(stdout,
                "INFRA: %llu failure(s) (test-harness infrastructure: work dir /"
                " source write / fork / exec — see tests/support/aot_support.hpp)\n",
                static_cast<unsigned long long>(
                    infraFailureCount().load(std::memory_order_relaxed)));
            std::fflush(stdout);
        });
        return true;
    }();
    (void)once;
}

// Record ONE infrastructure failure: count it, print the `[ INFRA ]` marker,
// and FAIL THE CURRENT TEST. `where` names the harness entry point, `reason`
// the stage and the errno.
inline void noteInfraFailure(const char* where, const std::string& reason);

struct InfraExpectation {
    InfraExpectation() : prev_(currentInfraExpectation()) {
        currentInfraExpectation() = this;
    }
    ~InfraExpectation() {
        currentInfraExpectation() = prev_;
        // Undo this scope's contribution to the process tally: a deliberate
        // probe is not a real outage, and the `INFRA:` line must stay true.
        if (count)
            infraFailureCount().fetch_sub(count, std::memory_order_relaxed);
    }
    InfraExpectation(const InfraExpectation&) = delete;
    InfraExpectation& operator=(const InfraExpectation&) = delete;

    std::uint64_t count = 0;
    std::string lastReason;
    std::string lastWhere;

private:
    InfraExpectation* prev_;
};

inline void noteInfraFailure(const char* where, const std::string& reason) {
    ensureInfraTallyAtExit();
    infraFailureCount().fetch_add(1, std::memory_order_relaxed);
    if (InfraExpectation* e = currentInfraExpectation()) {
        ++e->count;
        e->lastWhere = where ? where : "";
        e->lastReason = reason;
        return;   // self-test interception: observed, NOT a test failure
    }
    // Inline with the test's own output (BEZEL_EXPECT_* writes to std::cout),
    // so the marker lands next to the `[ FAIL ]` line in the shard log.
    std::cout << "    [ INFRA ] " << (where ? where : "runAot") << ": "
              << reason << "\n"
              << "    [ INFRA ] the harness could not RUN this program: an "
                 "INFRASTRUCTURE failure (disk / fork / exec), NOT a compiler "
                 "rejection. The test is FAILED so no negative assertion can "
                 "read it as green (#3007).\n";
    std::cout.flush();
    ::bezel::test::currentTestFailed() = true;
}

// ---- forked in-process compile -------------------------------------------
enum class CompileOutcome { Ok, Rejected, Crashed, TimedOut, Infra };

struct CompileStatus {
    CompileOutcome outcome = CompileOutcome::Infra;
    std::string diag;   // captured driver stderr (bounded) or an infra message
    int signal = 0;     // valid when outcome == Crashed
};

// ── AOT link-concurrency token pool (#1780) ───────────────────────────────
// Every runAot compile forks a driver that invokes `clang` → `ld` — a PID- and
// RAM-heavy external process (~1-2 GiB peak each). Under the 16-shard runner —
// and worse, several dev agents each running the suite at once — the UNBOUNDED
// fan-out of concurrent clang/ld links exhausts fork/PID/RAM. That is exactly
// what makes the sharded runner spuriously over-report crashes on innocent
// tests: the compile step, not the program, ran out of resources.
//
// This bounds the number of CONCURRENT compiles MACHINE-WIDE — across every
// shard AND every agent's test run — with an flock-based counting semaphore:
// N token files, each acquired with a non-blocking exclusive flock. The kernel
// releases an flock when the holding fd is closed OR the holding process dies,
// so a crashed/SIGKILLed/OOM-reaped compile NEVER leaks a token (the failure
// mode a POSIX/SysV semaphore has, and the reason flock is used here). The
// token is held in the PARENT across the whole compile+link and released on
// return, so it is robust even when the forked child crashes.
//
// N defaults to min(PHYSICAL cores, RAM/1.5 GiB) and is overridable via
// BEZEL_AOT_LINK_TOKENS (0 disables the pool — e.g. a single-process run where
// the bound only adds latency). PHYSICAL, not logical, cores: a compile+link is
// largely single-threaded and RAM-heavy, so SMT/hyperthread siblings add ~1.2x
// throughput, not 2x — sizing to logical cores oversubscribes CPU and RAM.
// (Measured on a 10-physical/20-logical box: sharded suite 125s at N=10 vs 119s
// at N=15 vs 122s unbounded — the logical-core default bought nothing and only
// raised the RAM/PID ceiling.) The RAM clamp keeps a many-core/small-RAM host
// from OOMing on N concurrent ~1.5 GiB links. This is the machine-wide knob for
// "control test parallelism when multiple agents run the suite together": when
// several agents run the suite at once, set BEZEL_AOT_LINK_TOKENS LOWER so the
// TOTAL concurrent-compile count across all of them stays bounded.

// Physical (not hyperthread) core count — the useful concurrency ceiling for a
// CPU+RAM-heavy compile/link. Derived from /proc/cpuinfo as
// logical * (cpu_cores / siblings) so it is correct across sockets; falls back
// to logical/2 (assume 2-way SMT) if /proc/cpuinfo is unreadable.
inline int physicalCoreCount() {
    unsigned logical = std::thread::hardware_concurrency();
    if (logical == 0) logical = 8;
    int cpuCores = 0, siblings = 0;
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (in && (cpuCores == 0 || siblings == 0) && std::getline(in, line)) {
        auto field = [&](const char* key) -> int {
            if (line.rfind(key, 0) != 0) return 0;
            auto pos = line.find(':');
            return pos == std::string::npos ? 0 : std::atoi(line.c_str() + pos + 1);
        };
        if (cpuCores == 0) cpuCores = field("cpu cores");
        if (siblings == 0) siblings = field("siblings");
    }
    if (cpuCores > 0 && siblings > 0) {
        int phys = static_cast<int>(logical) * cpuCores / siblings;
        return phys < 1 ? 1 : phys;
    }
    int phys = static_cast<int>(logical) / 2;   // no topology → assume 2-way SMT
    return phys < 1 ? 1 : phys;
}

// Cap from available RAM: each concurrent compile+link peaks ~1.5 GiB, so this
// keeps a many-core/small-RAM box from OOMing. Returns a large number when
// MemAvailable is unreadable, so the core count governs by default.
inline int memoryCompileCap() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    long kib = 0;
    while (in >> key) {
        if (key == "MemAvailable:") { in >> kib; break; }
        std::string rest;
        std::getline(in, rest);
    }
    if (kib <= 0) return 1 << 20;   // unknown → do not bind on memory
    int cap = static_cast<int>((kib / 1024.0 / 1024.0) / 1.5);
    return cap < 2 ? 2 : cap;
}

inline int aotLinkTokenCount() {
    static const int v = [] {
        if (const char* e = std::getenv("BEZEL_AOT_LINK_TOKENS")) {
            if (*e != '\0') { int n = std::atoi(e); return n < 0 ? 0 : n; }
        }
        int phys = physicalCoreCount();
        int mem = memoryCompileCap();
        int n = phys < mem ? phys : mem;
        return n < 1 ? 1 : n;
    }();
    return v;
}

// RAII holder for one link-pool token (#1780). Ctor blocks (polling a
// non-blocking flock) until a token is free; dtor releases it by closing the
// fd. Copy is deleted — the fd (hence the lock) has single ownership.
//
// CRASH ROBUSTNESS. An flock is released by the KERNEL when the holding fd is
// closed OR the holding process dies — for ANY reason: SIGSEGV, SIGKILL, the
// OOM-killer, an assertion abort, a normal exit. So a process that dies while
// holding a token NEVER leaks it (this is exactly why flock is used and not a
// POSIX/SysV semaphore, which a crash DOES leak). That covers a crashing shard
// outright. Two guards close the only remaining windows:
//
//   • dropInChild() — after fork(), a token fd is INHERITED by the compile
//     child (fork duplicates fds → both refer to the same open-file-description,
//     and an flock is held on the OFD until *every* fd on it closes). If the
//     PARENT died mid-compile, the orphaned child's inherited copy would keep
//     the OFD — hence the lock — alive until that child finally exits. The
//     child drops its copy immediately, making the parent's fd the SOLE holder,
//     so a parent death releases the token at once regardless of the child.
//
//   • fail-open deadline — if no token frees within a deadline far beyond any
//     legitimate wait (a holder is bounded by the compile timeout, so a token
//     can't be held longer than that), stop waiting and proceed UNBOUNDED. The
//     pool is a resource optimization, not a correctness requirement, so
//     failing open (throttling degrades) beats failing closed (suite wedges).
//     With the guard above this should be unreachable; it is a logged backstop.
class LinkToken {
public:
    LinkToken() {
        int n = aotLinkTokenCount();
        if (n <= 0) return;  // pool disabled → no bound
        const std::string dir = "/var/tmp/bezel-scratch/aot-link-tokens";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const int failOpenSec = std::max(600, aotCompileTimeoutSec() * 5);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(failOpenSec);
        // Poll the N token files with a NON-blocking exclusive flock until one
        // is free. Because a held token is released only by fd-close or process
        // death, this always converges even if a prior run was killed mid-link
        // (no stale-token recovery step needed — the kernel already did it).
        for (;;) {
            for (int i = 0; i < n; ++i) {
                std::string p = dir + "/tok." + std::to_string(i);
                int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
                if (fd < 0) continue;  // dir race / fd exhaustion → try next
                if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { fd_ = fd; return; }
                ::close(fd);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // All N held longer than any holder can legitimately live.
                // Degrade rather than wedge: proceed with no token.
                std::fprintf(stderr,
                    "runAot: link-pool starved > %ds; proceeding UNBOUNDED "
                    "(all %d tokens stuck?)\n", failOpenSec, n);
                return;  // fd_ stays -1 → no bound; dtor is a no-op
            }
            // All N held; back off briefly. 20 ms is negligible against a
            // multi-second compile+link, and the wait is bounded by the compile
            // timeout of whichever holder finishes first.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~LinkToken() { if (fd_ >= 0) ::close(fd_); }  // close() releases the flock

    // Called in the forked compile child: drop this process's INHERITED copy of
    // the token fd so the parent is the sole lock holder (see class comment).
    // Idempotent; safe to call before _Exit.
    void dropInChild() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    LinkToken(const LinkToken&) = delete;
    LinkToken& operator=(const LinkToken&) = delete;

private:
    int fd_ = -1;
};

// ── fork()-failure degradation: OBSERVABLE, never silent ──────────────────
// When fork() fails, forkedCompile falls back to an IN-PROCESS compile, losing
// the crash/hang isolation the fork provides: a front-end crash on that one test
// would then abort the WHOLE gtest shard (killing every other test in it)
// instead of being contained to ran=false. Historically this degradation was
// SILENT — so a lost-isolation shard death was indistinguishable from an
// unrelated flake, and the crux hypothesis behind the shard-authoritative gate
// (each test is process-isolated, so serial-order heap corruption is
// structurally impossible) would be quietly violated. This makes it loud, and —
// since #1861 promoted the shard-authoritative pass to the DEFAULT gate with no
// serial backstop — FATAL BY DEFAULT: process isolation is the safety basis for
// dropping serial, so it must never be silently lost. The behaviours are:
//   * a greppable per-occurrence marker on the harness stderr (captured in the
//     shard log), and
//   * a per-process total printed at exit, so the count survives even if the
//     individual markers scroll past, and
//   * by default a hard NON-RUN (the test does NOT compile un-isolated; it
//     surfaces as ran=false), so isolation is GUARANTEED, not merely observable.
// BEZEL_AOT_FORKFAIL_FATAL=0 opts back into the old graceful degradation (run
// un-isolated, but loudly) for a caller that would rather run than not-run.
inline std::atomic<std::uint64_t>& forkFailureCount() {
    // Leaked on purpose: the atexit hook below reads it at process teardown,
    // after ordinary function-local statics would already have been destroyed.
    static std::atomic<std::uint64_t>* c = new std::atomic<std::uint64_t>(0);
    return *c;
}

// FATAL BY DEFAULT (#1861): a fork() failure refuses the un-isolated in-process
// compile, so process isolation — the basis for the shard-authoritative gate
// dropping the serial pass — is never silently lost. fork() only fails under
// genuine resource exhaustion (PID/RAM), which the #1780 link-token pool already
// bounds, so this changes nothing in normal operation. Set
// BEZEL_AOT_FORKFAIL_FATAL=0 to opt back into the graceful (run un-isolated,
// loudly) fallback.
inline bool forkFailIsFatal() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORKFAIL_FATAL");
        if (!e || *e == '\0') return true;   // default: fatal
        return std::string(e) != "0";
    }();
    return v;
}

// Emit the loud marker + count one fork()-failure degradation. Call in the
// PARENT, BEFORE any fd 2 redirect, so the marker reaches the real harness
// stderr the shard log captures.
inline void noteForkFailureDegradation(int savedErrno) {
    static const bool once = [] {
        std::atexit([] {
            unsigned long long n =
                forkFailureCount().load(std::memory_order_relaxed);
            if (n > 0)
                std::fprintf(stderr,
                    "[aot-degraded #fork-fail] %llu test(s) in this process fell "
                    "back to an UN-ISOLATED in-process compile (fork() failed); "
                    "crash isolation was lost for those tests\n", n);
        });
        return true;
    }();
    (void)once;
    forkFailureCount().fetch_add(1, std::memory_order_relaxed);
    std::fflush(stderr);
    std::fprintf(stderr,
        "\n[aot-degraded #fork-fail] fork() failed (errno=%d: %s)%s\n",
        savedErrno, std::strerror(savedErrno),
        forkFailIsFatal()
            ? " -> test NOT run un-isolated (fatal by default; "
              "BEZEL_AOT_FORKFAIL_FATAL=0 to degrade instead)"
            : " -> DEGRADED to an in-process compile — crash isolation LOST for "
              "this test");
    std::fflush(stderr);
}

// Run one REAL driver step in a fork()ed CHILD (crash- and hang-isolated, qr1
// M1). The child's stderr (diagnostics + any clang link error) is captured,
// bounded, into the result. fork() failure is FATAL by default (#1861: refuse the un-isolated
// compile so process isolation is never silently lost — the basis for the
// shard-authoritative gate having no serial backstop); it is always LOUD
// (noteForkFailureDegradation), and BEZEL_AOT_FORKFAIL_FATAL=0 opts back into a
// graceful in-process (un-isolated) fallback — same policy runJit had.
//
// GENERIC over the driver call (`work`, returning the driver's success flag) so
// the three shapes the harness needs — compile+link in one step, compile-only,
// link-only — share ONE copy of the isolation, capture, deadline, token and
// fork-failure machinery. `producedPath` is the artifact the step must leave on
// disk; `product` names it in the "reported success but produced nothing"
// diagnostic.
template <typename Work>
inline CompileStatus forkedDriverStep(Work&& work,
                                      const std::string& producedPath,
                                      const char* product,
                                      const std::string& ccErrPath) {
    // Bound machine-wide concurrent compiles (#1780). Held for the whole
    // compile+link (both the forked and the fork-failure in-process path) and
    // released when this function returns — including on timeout/crash, since
    // the parent always reaps before returning. Acquired BEFORE the fork so the
    // front-end + clang/ld link all run under the one token. The split steps
    // take a token each, so the machine-wide bound is unchanged: a compile and
    // its link are sequential, never concurrent.
    LinkToken linkToken;

    auto rejectFrom = [](std::string err, int code) -> CompileStatus {
        if (err.empty())
            err = "runAot: driver rejected the program (exit " +
                  std::to_string(code) + ")";
        return {CompileOutcome::Rejected, std::move(err), 0};
    };

    std::fflush(nullptr);   // flush parent stdio so the child can't double-print
    std::cout.flush();

    pid_t pid = ::fork();
    if (pid < 0) {
        // fork() failed (e.g. under the very memory pressure this guards). Make
        // the degradation OBSERVABLE first (never silent) — the marker must hit
        // the harness stderr BEFORE the fd-2 redirect below captures it into the
        // per-test cc.err. errno is still fresh here, right after fork().
        const int forkErrno = errno;
        noteForkFailureDegradation(forkErrno);
        // Refuse to run un-isolated (isolation GUARANTEED — FATAL BY DEFAULT
        // since #1861): return without compiling so a front-end crash can't take
        // the shard. BEZEL_AOT_FORKFAIL_FATAL=0 opts into the fallback below.
        if (forkFailIsFatal())
            return {CompileOutcome::Infra,
                    "runAot: fork() failed (errno=" + std::to_string(forkErrno) +
                        ") — refusing an un-isolated in-process compile (fatal by "
                        "default; set BEZEL_AOT_FORKFAIL_FATAL=0 to degrade)",
                    0};
        // Fall back to an in-process compile so the test still runs — accepting
        // the loss of isolation on this one degraded path (same policy as runJit).
        int fd = ::open(ccErrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int saved = -1;
        if (fd >= 0) {
            std::fflush(stderr);
            saved = ::dup(STDERR_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        const bool ok = work();
        if (saved >= 0) {
            std::fflush(stderr);
            std::cerr.flush();
            ::dup2(saved, STDERR_FILENO);
            ::close(saved);
        }
        std::string err = slurp(ccErrPath);
        if (ok && fs::exists(producedPath)) return {CompileOutcome::Ok, err, 0};
        return rejectFrom(std::move(err), ok ? 0 : 1);
    }
    if (pid == 0) {
        // ---- CHILD ----
        // Drop the inherited link-token fd at once: the parent is the sole lock
        // holder, so a parent death releases the token immediately even if this
        // child is orphaned (#1780 crash robustness — see LinkToken).
        linkToken.dropInChild();
        // qr1 M4: fd 0 <- /dev/null (a front-end that read stdin can't block).
        int nullFd = ::open("/dev/null", O_RDONLY);
        if (nullFd >= 0) {
            ::dup2(nullFd, STDIN_FILENO);
            if (nullFd > STDERR_FILENO) ::close(nullFd);
        }
        // Capture the driver's diagnostics (std::cerr) AND the clang link
        // subprocess's stderr (inherited fd 2) into ccErr. Route stdout there
        // too so nothing leaks into the test runner's stdout.
        int fd = ::open(ccErrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) ::close(fd);
        }
        // Test-only fault injection for the crash-isolation regression test
        // (qr1 M1 / qr2 item 1; aot_crash_isolation_test.cpp); NEVER set in
        // normal runs. Proves a compiler crash in the child is isolated to
        // ran=false without killing the parent test binary. PR_SET_DUMPABLE=0
        // suppresses the (COW-copied, potentially large) core dump — robust vs
        // Fedora's systemd-coredump pipe, which ignores RLIMIT_CORE=0 — since the
        // crash is expected, not something to capture. Reset the handler to
        // default so the raise is fatal even if LLVM installed a crash handler.
        if (std::getenv("BEZEL_AOT_TEST_CRASH_HOOK")) {
            ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
            ::signal(SIGSEGV, SIG_DFL);
            ::raise(SIGSEGV);
        }

        const bool ok = work();
        std::fflush(stderr);
        std::cerr.flush();
        // _Exit (NOT exit): skip atexit handlers and static destructors in the
        // forked copy — the child's whole heap + LLVM allocator state is
        // reclaimed by the OS, keeping the parent's RSS flat (qr1 E1). Same
        // discipline as runJit's fork.
        std::_Exit(ok ? 0 : 1);
    }

    // ---- PARENT ----
    bool timedOut = false;
    int status = reapWithDeadline(pid, aotCompileTimeoutSec(), timedOut);
    std::string err = slurp(ccErrPath);  // bounded (M2)

    if (timedOut) {
        return {CompileOutcome::TimedOut,
                "runAot: compile did not finish within " +
                    std::to_string(aotCompileTimeoutSec()) +
                    "s; killed (compile hang / runaway front-end or link)" +
                    (err.empty() ? "" : "\n--- compiler stderr ---\n" + err),
                0};
    }
    if (WIFSIGNALED(status)) {
        // qr1 M1: the front-end/codegen crashed. In-process (unforked) this
        // would have aborted the whole gtest shard; isolated to the child it is
        // an isolated ran=false. Label it distinctly so triage sees a COMPILER
        // BUG (worth filing) rather than an ordinary program rejection.
        int sig = WTERMSIG(status);
        return {CompileOutcome::Crashed,
                "runAot: compiler CRASHED (signal " + std::to_string(sig) +
                    ") — isolated; the program was not run (likely a front-end/"
                    "codegen bug on this input)" +
                    (err.empty() ? "" : "\n--- compiler stderr ---\n" + err),
                sig};
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 0) {
        // qr1 N3: guard against the impossible-but-cheap case of a "success"
        // exit with no artifact (would otherwise run a missing/stale exe).
        if (!fs::exists(producedPath)) {
            return {CompileOutcome::Infra,
                    std::string("runAot: driver reported success but produced "
                                "no ") + product + " at " + producedPath, 0};
        }
        return {CompileOutcome::Ok, err, 0};
    }
    return rejectFrom(std::move(err), code);
}

// Compile+link in ONE forked driver call — the un-split path, used whenever the
// two-level cache is not in play (cache disabled, no sound identity, or the
// crash-hook affordance armed). Byte-for-byte the pre-split behaviour.
inline CompileStatus forkedCompile(const std::string& srcPath,
                                   const std::string& exePath,
                                   const codegen::CodegenOptions& opts,
                                   const std::string& ccErrPath) {
    return forkedDriverStep(
        [&] {
            Driver driver;
            return driver.compileFile(srcPath, exePath, /*deps=*/{}, opts)
                .success;
        },
        exePath, "executable", ccErrPath);
}

// The FRONT-END half, isolated identically: source → native object, no link.
inline CompileStatus forkedCompileObject(const std::string& srcPath,
                                         const std::string& objectPath,
                                         const codegen::CodegenOptions& opts,
                                         const std::string& ccErrPath) {
    return forkedDriverStep(
        [&] {
            Driver driver;
            return driver
                .compileExecutableObject(srcPath, objectPath, /*deps=*/{}, opts)
                .success;
        },
        objectPath, "object", ccErrPath);
}

// The LINK half, isolated identically: object → executable. Forked for the same
// reason the compile is — a wedged `clang`/`ld` must hit the compile deadline
// and be SIGKILLed rather than stall the shard to its 300s cap.
inline CompileStatus forkedLink(const std::string& objectPath,
                                const std::string& exePath,
                                const codegen::CodegenOptions& opts,
                                const std::string& ccErrPath) {
    return forkedDriverStep(
        [&] {
            Driver driver;
            return driver.linkExecutableObject(objectPath, exePath, /*deps=*/{},
                                               opts);
        },
        exePath, "executable", ccErrPath);
}

// ── Content-addressed compiled-test cache (the "link-cache") ───────────────
// A single gate compiles every AOT test 2-3x under the SAME compiler state
// (the sharded pass, the serial confirmation, and any flake/arbitration
// reruns). The compile+link — a real `clang` → `ld`, ~1-2 GiB and the dominant
// cost of an AOT test — is a PURE function of its inputs, so a SUCCESSFUL link
// product can be reused across those repeats, collapsing them to 1x. This is a
// test-infra optimization only: a cache hit runs the very same executable the
// real compiler would have produced, fork+exec-isolated EXACTLY as today.
//
// KEY (SHA-256 over):
//   (a) COMPILER IDENTITY — libbezel_core.a (BEZEL_CORE_LIB_PATH), the k2
//       refinement (#2100) of the original whole-binary hash. The compile runs
//       IN-PROCESS (Driver::compileFile) inside bezel_tests, and every byte of
//       that path — front-end, sema, codegen, emitObjectFile, AND the driver's
//       clang→ld link recipe — is linked out of bezel_core; if the archive's
//       bytes are unchanged, the compiler code path is byte-identical
//       regardless of which TEST objects were relinked into bezel_tests. So a
//       test-only edit (every "fix + regression test" iteration) keeps the
//       cache WARM, where the old /proc/self/exe key went cold on ANY relink.
//       STALE-CORE GUARD: if core.a on disk is NEWER than the running binary,
//       the in-process compiler may predate it (e.g. `--target bezelc` rebuilt
//       core.a without relinking bezel_tests, or a relink replaced the binary
//       mid-run) — keying THIS process's products under the new archive's hash
//       could hand a future, really-rebuilt bezel_tests a stale hit. The
//       identity then falls back to /proc/self/exe (the k1 behaviour):
//       conservative, never stale. Same fallback when the macro is undefined
//       or the archive unreadable. Known gaps: the external clang/ld toolchain
//       and the dynamically-linked libLLVM.so are now folded in via the
//       `toolchain` identity (per #3006), so a system toolchain update
//       auto-invalidates the cache instead of needing a manual wipe. Residual:
//       the system libs the link resolves (libc/libm/libpthread/libstdc++) are
//       still untracked — a glibc/libstdc++-only update (no clang/ld/libLLVM
//       change) is not detected; pair such an update with a cache wipe
//       (gc_scratch.sh reaps the dir anyway).
//   (b) RUNTIME-LIBRARY IDENTITY — libbezel_runtime.a (BEZEL_RUNTIME_LIB_PATH),
//       what the driver's link actually consumes. Under k2 this is LOAD-BEARING
//       (the runtime is whole-archived into bezel_tests, not into core.a, so
//       (a) no longer covers it): a runtime-only rebuild must invalidate cached
//       link products, and hashing the .a directly guarantees it. Because it
//       is load-bearing, an UNREADABLE archive fails CLOSED — the cache
//       disables itself for the process instead of minting under-keyed
//       entries. (If the macro is somehow undefined, the core branch is
//       compiled out entirely and the identity is the whole test binary,
//       which contains the archived runtime — sound, just coarser.)
//   (c) COMPILE/LINK MODE — the CodegenOptions bits that change the emitted
//       object AND the link line: sanitizeAddress (plain vs --sanitize=address
//       → -fsanitize=address -g), checkedDisposal (§28.2 guards), isMainModule.
//       Each mode is a DISTINCT artifact and keys separately, so a plain, an
//       ASan, and a checked-disposal build of the same source never collide.
//   (d) the PROGRAM SOURCE TEXT — exactly the bytes writeSource() wrote.
//
// VALUE: the linked executable ONLY. Only SUCCESSFUL compile+link products are
// cached; every reject / compiler-crash / compile-timeout path runs the REAL
// compiler so its diagnostics stay live (the oracle is exact where it matters).
// Nothing on the runAot/runAotChecked SUCCESS path consumes a compile-time side
// output: the captured compiler stderr is DISCARDED on CompileOutcome::Ok, the
// driver deletes the .o after linking, and no metadata file is read back — so
// the executable is the whole product and caching it is observationally exact.
//
// STORE: executables at <cacheDir>/<first2>/<hash>, objects at
// <cacheDir>/o/<first2>/<hash>, link products at <cacheDir>/l/<first2>/<hash>,
// installed atomically (hardlink|copy to a temp
// in the same dir, then rename) so concurrent shards race benignly. A hit
// hardlinks (or, across a filesystem boundary, copies) the cached exe into the
// test's workdir and runs it as a fresh build would. A hit performs NO link, so
// it does NOT consume a #1780 LinkToken.
//
// SPLIT (k3): the key above is now the FIRST of three lookups — see the "two-
// level split" comment on computeObjectKey/computeLinkKey below for the object
// and link keys that make a compiler-only or runtime-only rebuild keep half the
// store warm. This key's composition is deliberately UNCHANGED so entries
// stored before the split are still served.
//
// KNOBS: BEZEL_AOT_CACHE=0 disables (default ON); BEZEL_AOT_CACHE_DIR overrides
// the location; BEZEL_AOT_CACHE_STATS=1 prints a hits/misses line at process
// (shard) exit. BEZEL_DUMP_IR bypasses the cache entirely (a served product
// cannot print the IR the flag asks for). GC: once per process, under an flock so only one shard sweeps,
// if the store exceeds the cap (BEZEL_AOT_CACHE_MAX_GB, default 64 GiB — sized
// so full-suite ASan + plain products persist across gates, the point of the
// k2 identity) the oldest-mtime entries are deleted down to 3/4 of the cap
// (scripts/gc_scratch.sh may also reap the directory).

// Default 0755 for an installed/stored executable.
inline constexpr fs::perms kExePerms =
    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
    fs::perms::others_read | fs::perms::others_exec;

inline bool cacheEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_CACHE");
        return !(e && std::string(e) == "0");  // default ON
    }();
    return v;
}


// SHA-256 the contents of `path` into `out`. Streams in chunks so a large
// artifact (the test binary, the runtime archive) never lands wholesale in RAM.
inline bool sha256File(const std::string& path, std::array<uint8_t, 32>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    llvm::SHA256 h;
    char buf[1 << 16];
    while (in) {
        in.read(buf, sizeof(buf));
        std::streamsize got = in.gcount();
        if (got > 0)
            h.update(llvm::ArrayRef<uint8_t>(
                reinterpret_cast<const uint8_t*>(buf),
                static_cast<std::size_t>(got)));
    }
    if (in.bad()) return false;
    out = h.final();
    return true;
}

// per #3006: the EXTERNAL toolchain identity — clang/ld (the link driver and
// linker the driver shells out to via std::system) and libLLVM.so (the
// dynamically-linked codegen backend the IN-PROCESS compiler calls). These are
// inputs to the build that the archive identities do NOT cover: a Fedora clang /
// binutils / llvm-libs update swaps the on-disk tools the cached products were
// produced with, but leaves libbezel_core.a / libbezel_runtime.a byte-identical
// (core.a is only rebuilt if ccache misses on the new headers — and a same-soname
// llvm-libs update can change libLLVM.so WITHOUT touching the devel headers,
// so core.a is served stale and the in-process codegen silently changes). The
// documented gap (aot_support.hpp KEY comment, and the K2 comment below) is now
// closed by folding this digest into every key (see BuildIdentity).
//
// CHEAP: two popen subprocesses (clang --version, ld --version) + one
// /proc/self/maps read + one stat, ONCE per process (cached in buildIdentity's
// static). clang --version and ld --version each return in a few ms; the maps
// read is a single open. This never dominates even a cold build.
//
// FAIL CLOSED. clang is the one tool the link cannot proceed without (the
// driver's recipe is literally "clang …"), so an unidentified clang ⇒ no sound
// toolchain identity ⇒ the cache disables itself for the process (returns ""),
// exactly like the existing unreadable-archive behaviour — never mint an
// under-keyed entry. ld and libLLVM are BEST-EFFORT: ld adds the binutils signal
// (a separate package from clang), and libLLVM is OPTIONAL because a STATIC-LLVM
// build folds the backend into core.a (already keyed) — no libLLVM line in
// /proc/self/maps means the backend is not a separate .so, so its absence is
// sound rather than a gap.
inline bool toolchainIdentityFrom(std::string_view clangVer,
                                  std::string_view ldVer,
                                  std::string_view llvmInfo,
                                  std::array<uint8_t, 32>& out) {
    if (clangVer.empty()) return false;  // clang unidentified → fail CLOSED
    llvm::SHA256 h;
    static const char kSep[] = "\0";  // NUL keeps adjacent fields unambiguous
    h.update(llvm::StringRef(clangVer.data(), clangVer.size()));
    h.update(llvm::StringRef(kSep, 1));
    if (!ldVer.empty()) {
        h.update(llvm::StringRef(ldVer.data(), ldVer.size()));
        h.update(llvm::StringRef(kSep, 1));
    }
    if (!llvmInfo.empty()) {
        h.update(llvm::StringRef(llvmInfo.data(), llvmInfo.size()));
        h.update(llvm::StringRef(kSep, 1));
    }
    out = h.final();
    return true;
}

// The live toolchain strings, gathered once. clang/ld via popen (the shell PATH
// is exactly what Driver's std::system link resolves, so this is the same clang
// whose output the cache would serve). libLLVM via /proc/self/maps: the first
// mapping whose path names libLLVM is the shared backend the compiler actually
// loaded; its size + mtime (NUL-joined onto the path) is stable across relinks
// of bezel_tests (the system .so is untouched by a compiler rebuild) but moves
// on a dnf update that replaces the .so (new mtime/size) — the exact signal
// #3006 wants.
inline bool computeToolchainIdentity(std::array<uint8_t, 32>& out) {
    auto capture = [](const char* cmd) -> std::string {
        std::string result;
        // popen goes through the shell, matching Driver::linkExecutableObject's
        // std::system — so it resolves the same PATH clang/ld the link uses.
        if (FILE* p = ::popen(cmd, "r")) {
            char buf[256];
            while (fgets(buf, sizeof(buf), p)) result += buf;
            ::pclose(p);
        }
        return result;
    };
    const std::string clangVer = capture("clang --version 2>&1");
    const std::string ldVer = capture("ld --version 2>&1");
    // libLLVM: the COMPILER's codegen backend. The path comes from the process's
    // own memory map, so it is the actually-loaded .so (not a build-time guess).
    // A static-LLVM build has no such line → llvmInfo empty → sound (backend is
    // inside core.a, already keyed).
    std::string llvmInfo;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            // The maps path is the last whitespace-delimited token on the line.
            if (line.find("libLLVM") == std::string::npos) continue;
            auto sp = line.find_last_of(" \t");
            if (sp == std::string::npos) break;
            const std::string path = line.substr(sp + 1);
            if (path.empty() || path[0] != '/') break;
            std::error_code ec;
            const auto sz = fs::file_size(path, ec);
            if (ec) break;
            const auto mt = fs::last_write_time(path, ec);
            if (ec) break;
            llvmInfo = path;
            llvmInfo += '|';
            llvmInfo += std::to_string(sz);
            llvmInfo += '|';
            llvmInfo += std::to_string(mt.time_since_epoch().count());
            break;  // the first libLLVM mapping is the backend
        }
    }
    return toolchainIdentityFrom(clangVer, ldVer, llvmInfo, out);
}

// The per-process build identity, computed ONCE (hashing the ~100 MB core
// archive or the test binary is a few hundred ms; amortized over the whole
// shard it is negligible).
//
// `compiler` and `runtime` are kept SEPARATE — that separation is what lets the
// two-level cache key each build half on its own inputs (see the "k3" comment
// below). `combined` folds compiler ‖ runtime ‖ toolchain into the KE identity.
// (Pre-#3006 `combined` was SHA-256(compiler ‖ runtime) only — the k2 identity.
// Folding in toolchain deliberately shifts EVERY exe key: entries stored before
// the toolchain was tracked were built under an UNTRACKED external clang/ld/
// libLLVM, so they cannot be proven sound against the current toolchain and are
// orphaned — never hit — rather than risked. The schema tag stays "k2;": a
// different hash (toolchain now in the input) is a different key with no
// collision, so no tag bump is needed; the orphans fall to the size GC.)
struct BuildIdentity {
    bool usable = false;        // false => no sound identity; cache disabled
    bool splitUsable = false;   // false => the two-level (k3) path is unsound here
    std::array<uint8_t, 32> compiler{};   // core.a (guarded) or /proc/self/exe
    std::array<uint8_t, 32> runtime{};    // libbezel_runtime.a; valid iff splitUsable
    std::array<uint8_t, 32> toolchain{};  // per #3006: clang/ld/libLLVM; required
    std::array<uint8_t, 32> combined{};   // SHA-256(compiler ‖ runtime ‖ toolchain)
};

inline const BuildIdentity& buildIdentity() {
    // Leaked on purpose: read from cache paths that may run late in teardown.
    static const BuildIdentity* id = [] {
        auto* out = new BuildIdentity();
        llvm::SHA256 h;
        bool haveRuntime = false;
        std::array<uint8_t, 32> rtHash{};
#ifdef BEZEL_RUNTIME_LIB_PATH
        // (b) the runtime archive is hashed FIRST and its readability is
        // REQUIRED: under the k2 identity it is load-bearing (core.a does not
        // contain the runtime), and even under the exe fallback the product's
        // link consumes the on-disk .a — so an unreadable archive (e.g. the
        // ENOENT window of ninja's `rm -f lib && ar qc` rebuild) means no
        // sound identity exists for products linked NOW. Fail CLOSED (cache
        // disabled for this process) rather than mint under-keyed entries a
        // different runtime could later stale-hit.
        if (!sha256File(BEZEL_RUNTIME_LIB_PATH, rtHash)) return out;  // unusable
        haveRuntime = true;
        // SOUNDNESS GUARD (new with the split): the harness hashes the archive
        // at ITS macro, but the DRIVER links the archive at the macro compiled
        // into bezel_core. CMake sets both from the same generator expression,
        // so they agree — but if a future build ever let them diverge, the
        // harness would be keying on an archive the link never consumed, i.e.
        // exactly the stale-hit this key exists to prevent. Verify, and fail
        // CLOSED (no cache at all) if they disagree.
        if (Driver::runtimeLibraryPath() != std::string(BEZEL_RUNTIME_LIB_PATH))
            return out;  // unusable
#endif
        bool haveCoreIdentity = false;
#if defined(BEZEL_CORE_LIB_PATH) && defined(BEZEL_RUNTIME_LIB_PATH)
        // (a) k2: the compiler archive, guarded — only trusted when the
        // running binary is at least as new as the archive, i.e. it was linked
        // FROM these bytes (see the STALE-CORE GUARD note in the KEY comment).
        // /proc/self/exe resolves to the on-disk binary's inode even after a
        // mid-run relink replaced the path (the magic link pins the deleted
        // inode, whose old mtime then correctly trips this guard). The branch
        // requires BOTH archive macros: with the runtime path unknown a
        // core-only identity would leave the runtime entirely unkeyed, so the
        // exe fallback (which contains the whole-archived runtime) is used
        // instead. TOCTOU hardening: core.a is re-stat'ed AFTER hashing — a
        // rebuild that began mid-hash changes the mtime and drops us to the
        // exe fallback rather than keying a torn/new archive under a passed
        // guard.
        {
            std::error_code ecCore, ecExe, ecCore2;
            const auto coreTime =
                fs::last_write_time(BEZEL_CORE_LIB_PATH, ecCore);
            const auto exeTime = fs::last_write_time("/proc/self/exe", ecExe);
            if (!ecCore && !ecExe && coreTime <= exeTime) {
                std::array<uint8_t, 32> coreHash;
                if (sha256File(BEZEL_CORE_LIB_PATH, coreHash) &&
                    fs::last_write_time(BEZEL_CORE_LIB_PATH, ecCore2) ==
                        coreTime &&
                    !ecCore2) {
                    out->compiler = coreHash;
                    haveCoreIdentity = true;
                }
            }
        }
#endif
        if (!haveCoreIdentity) {
            // k1 fallback: the whole test binary (which contains the
            // whole-archived runtime). Conservative, never stale.
            std::array<uint8_t, 32> exeHash;
            if (!sha256File("/proc/self/exe", exeHash)) return out;  // unusable
            out->compiler = exeHash;
        }
        // per #3006: the EXTERNAL toolchain (clang/ld/libLLVM). clang is
        // REQUIRED — the link shells out to it, so no clang identity means no
        // sound toolchain identity, and the cache disables itself for the
        // process (fail CLOSED) rather than serve products from an untracked
        // toolchain. This mirrors the existing fail-closed on an unreadable
        // runtime archive: never mint an under-keyed entry.
        //
        // FORK-SAFE — why #3006 is split_build-safe alongside #1608. This whole
        // lambda runs in the PARENT: the static above is initialised on the
        // first cache lookup, which happens in cachedCompile BEFORE
        // forkedDriverStep forks, so every compile/link child INHERITS the
        // already-computed identity and never recomputes it (no popen, no
        // /proc/self/maps read inside a child). split_build's two forked builds
        // therefore see the SAME toolchain digest, and the digest is a pure KEY
        // input that never reaches emitted bytes. The earlier #3006 lane broke
        // split_build only because its base (04b5279) predates #1608 and ran
        // the compiles IN-PROCESS, where this gather's popen state churn
        // amplified #1608's in-process-compile non-determinism; #1608's
        // fork-isolation removed that mechanism entirely. // per #3006 / #1608
        std::array<uint8_t, 32> tcHash;
        if (!computeToolchainIdentity(tcHash)) return out;  // unusable — fail CLOSED
        out->toolchain = tcHash;
        h.update(llvm::ArrayRef<uint8_t>(out->compiler.data(),
                                         out->compiler.size()));
        if (haveRuntime) {
            // (b): fold in the runtime archive the driver's link consumes.
            out->runtime = rtHash;
            h.update(llvm::ArrayRef<uint8_t>(rtHash.data(), rtHash.size()));
        }
        // Fold the toolchain into `combined` so the KE (whole-exe) key carries
        // it; K1/K2 fold it in via their own composers (the emitted object
        // depends on the libLLVM backend, the link on clang/ld).
        h.update(llvm::ArrayRef<uint8_t>(tcHash.data(), tcHash.size()));
        out->combined = h.final();
        out->usable = true;
        // The two-level split is only sound when the runtime archive the link
        // consumes is KNOWN and hashed: the link key's whole job is to make a
        // runtime change invalidate link products, and with no runtime path
        // there is nothing to key on. (That configuration cannot link a working
        // Bezel program at all — the bz_* symbols would be undefined — so this
        // is a belt-and-braces guard, not a live path.) Without it the single
        // k2 exe key still applies, unchanged.
        out->splitUsable = haveRuntime;
        return out;
    }();
    return *id;
}

// SHA-256(compiler ‖ runtime ‖ toolchain) — the k2 exe key's identity
// component (per #3006 the toolchain — clang/ld/libLLVM — is folded in).
// Returns nullptr if no identity input can be hashed: with no sound compiler
// identity the cache DISABLES itself rather than risk a stale hit.
inline const std::array<uint8_t, 32>* combinedIdentity() {
    const BuildIdentity& id = buildIdentity();
    return id.usable ? &id.combined : nullptr;
}

// The mode component of the key (c): every CodegenOptions bit that alters the
// emitted object or the link line. The schema tag is bumped if the key
// composition ever changes, so old entries fall out cleanly — "k1;"→"k2;" with
// the core-archive identity refinement (#2100): a k1 entry (exe-hash identity)
// must never be served under a k2 key or vice versa. #3006 folds the external
// toolchain into every key WITHOUT a tag bump: the toolchain digest is a new
// hash INPUT, so every key's SHA-256 shifts and pre-#3006 entries (built under
// an untracked toolchain) are orphaned — never hit, no collision — and GC'd,
// which is the property a bump would buy. See BuildIdentity for the rationale.
inline std::string modeString(const codegen::CodegenOptions& opts) {
    std::string s = "k2;";
    s += "sa=";  s += opts.sanitizeAddress ? '1' : '0';
    s += ";cd="; s += opts.checkedDisposal ? '1' : '0';
    s += ";mm="; s += opts.isMainModule ? '1' : '0';
    // Proposal 0082 stage-1 prototype (#3048): a FRONT-END desugaring, but one
    // that changes the emitted object (a program that did not compile now
    // does), so it belongs in the key like any codegen-mode bit.
    s += ";sab="; s += opts.scopedAwaitBind ? '1' : '0';
    s += ';';
    return s;
}

inline std::string toHex(const std::array<uint8_t, 32>& d) {
    static const char* k = "0123456789abcdef";
    std::string s(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        s[2 * i]     = k[d[i] >> 4];
        s[2 * i + 1] = k[d[i] & 0xF];
    }
    return s;
}

// The full cache key hex, or "" when no sound compiler identity is available
// (→ the caller compiles for real rather than risk a stale hit).
inline std::string computeCacheKey(std::string_view source,
                                   const codegen::CodegenOptions& opts) {
    const auto* ident = combinedIdentity();
    if (!ident) return {};
    llvm::SHA256 h;
    h.update(llvm::ArrayRef<uint8_t>(ident->data(), ident->size()));
    const std::string mode = modeString(opts);
    h.update(llvm::StringRef(mode.data(), mode.size()));
    h.update(llvm::StringRef(source.data(), source.size()));
    return toHex(h.final());
}

// ── The two-level ("k3") split of the same cache ───────────────────────────
// WHY. Under the single k2 key above, the compiler archive and the runtime
// archive are folded into ONE identity, so ANY src/ change invalidates EVERY
// stored entry — and since ~92% of this project's src/ commits touch the
// compiler while ~8% touch only the runtime, essentially every landing
// re-cools the whole gate. The two archives are not, however, inputs to the
// same build step:
//
//   object  = compileExecutableObject(source)   depends on: COMPILER, opts, source
//   exe     = linkExecutableObject(object)      depends on: object, RUNTIME, recipe
//
// VERIFIED, not assumed: nothing under src/codegen, src/sema, src/ownership or
// src/module includes a runtime header or names BEZEL_RUNTIME_LIB_PATH; the
// runtime archive appears exactly once in the compiler, in Driver's link
// command (Driver::linkCommandLine). Every ABI/layout constant codegen shares
// with the runtime is a literal compiled INTO bezel_core, hence covered by the
// compiler identity. So the emitted object is independent of the runtime
// archive's bytes, and the split is exact — not an approximation.
//
// Keying each half on its own inputs therefore gives:
//   * a runtime-only rebuild  → every cached OBJECT stays valid (no front-end
//     work at all); only links re-run.
//   * a compiler-only rebuild → every cached LINK stays valid for each object
//     that came out byte-identical (i.e. all but the handful of tests the
//     change actually affects), so the gate pays codegen but not the link.
//
// THE THREE KEYS (all SHA-256, all hex):
//   KE ("k2;", unchanged) = H(compiler ‖ runtime ‖ mode ‖ source) → exe.
//        The unchanged fast path: one hash, one hardlink, no object I/O, and —
//        because its composition is untouched — every entry stored by the
//        pre-split harness is still served.
//   K1 ("k3o;") = H(compiler ‖ codegen-mode ‖ stdlib-prelude ‖ source) → object.
//        The prelude (Driver::preludeSourceFingerprint) is the text the driver
//        injects into EVERY compilation unit, so it is as much an input to the
//        emitted object as the user's own source. It is keyed explicitly even
//        though the compiler archive currently contains it — see composeObjectKey.
//   K2 ("k3l;") = H(object-CONTENT-hash ‖ runtime ‖ link-recipe) → exe.
//
// WHY K2 KEYS ON THE RECIPE, NOT ON THE COMPILER. The link line itself is
// compiler code (Driver::linkCommandLine), so a link key that ignored the
// compiler would be stale after an edit that changes a link flag while leaving
// codegen alone. Folding the whole compiler archive in would be sound but would
// destroy the entire benefit (every compiler landing would re-cool links too).
// K2 instead hashes the RECIPE — the actual command line, with only the
// per-invocation object/exe paths and the runtime PATH abstracted
// (Driver::linkRecipeSignature) — which is precisely the compiler-side input
// the link depends on, and is derived from the same function that runs the
// link, so it cannot drift.
//
// WHAT K2 STILL DOES NOT COVER: the system libraries the link resolves
// (libc/libm/libpthread/libstdc++) are unhashed — a glibc/libstdc++-only
// update (no clang/ld/libLLVM change) is not detected. The external clang/ld
// toolchain and the ASan runtime (part of the clang/compiler-rt package) ARE
// now covered via the `toolchain` identity folded in per #3006, so a clang or
// binutils update invalidates cached executables automatically. The system-lib
// residual is unchanged from k2; pair such an update with a cache wipe.

// The codegen-mode component of K1: every CodegenOptions bit that alters the
// EMITTED OBJECT. All of them do (sanitizeAddress adds the AddressSanitizer
// pass, checkedDisposal emits §28.2 guards, isMainModule decides whether a
// native `main` is lowered, scopedAwaitBind runs the #3048 front-end
// desugaring), so this is modeString's set under its own schema tag —
// kept as a separate function so that if the two ever diverge (an option that
// changes only the link line, say), each key can move independently.
inline std::string codegenModeString(const codegen::CodegenOptions& opts) {
    std::string s = "k3o;";
    s += "sa=";  s += opts.sanitizeAddress ? '1' : '0';
    s += ";cd="; s += opts.checkedDisposal ? '1' : '0';
    s += ";mm="; s += opts.isMainModule ? '1' : '0';
    s += ";sab="; s += opts.scopedAwaitBind ? '1' : '0';   // #3048, see modeString
    s += ';';
    return s;
}

// Driver::linkRecipeSignature, memoized per ASan mode (it is a pure function of
// one bool, but it builds a string and is consulted on every cache lookup).
inline const std::string& linkRecipeSignature(bool sanitizeAddress) {
    static const std::string* plain =
        new std::string(Driver::linkRecipeSignature(false));
    static const std::string* asan =
        new std::string(Driver::linkRecipeSignature(true));
    return sanitizeAddress ? *asan : *plain;
}

// K1's composition, as a PURE function of its inputs — the process identity is
// supplied by the caller. Split out from computeObjectKey so the cache-key
// correctness test (tests/support/aot_cache_key_test.cpp) can vary one input at
// a time and assert what does and does not move, without needing to doctor a
// real archive on disk.
//
// `preludeText` is the auto-imported stdlib prelude the driver injects into
// every compilation unit (Driver::preludeSourceFingerprint). It is a genuine
// codegen input — the same user source compiled against a different prelude can
// emit a different object — and is keyed EXPLICITLY even though today it also
// lives inside the compiler archive that `compilerId` hashes. The redundancy is
// deliberate: the archive covers it only because the text happens to be a
// compiled-in constant, and a refactor that loaded the stdlib from disk would
// silently drop that coverage while every test still passed. Keying the text
// directly makes the property survive that refactor.
inline std::string composeObjectKey(const std::array<uint8_t, 32>& compilerId,
                                    const std::string& codegenMode,
                                    std::string_view preludeText,
                                    std::string_view source,
                                    const std::array<uint8_t, 32>& toolchainId) {
    llvm::SHA256 h;
    h.update(llvm::ArrayRef<uint8_t>(compilerId.data(), compilerId.size()));
    h.update(llvm::StringRef(codegenMode.data(), codegenMode.size()));
    // Length-prefixed so no prelude/source boundary shuffle can collide.
    const std::string plen = std::to_string(preludeText.size()) + ";";
    h.update(llvm::StringRef(plen.data(), plen.size()));
    h.update(llvm::StringRef(preludeText.data(), preludeText.size()));
    h.update(llvm::StringRef(source.data(), source.size()));
    // per #3006: the in-process codegen calls into libLLVM.so, so the emitted
    // object depends on the toolchain backend. A same-soname llvm-libs update
    // can swap that .so without rebuilding core.a (whose hash `compilerId`
    // covers), so without this component a stale object would be served and
    // then linked into a stale executable — the false-green this key exists to
    // prevent.
    h.update(llvm::ArrayRef<uint8_t>(toolchainId.data(), toolchainId.size()));
    return toHex(h.final());
}

// Driver::preludeSourceFingerprint, memoized (it is consulted on every lookup).
inline const std::string& preludeFingerprint() {
    static const std::string* v =
        new std::string(Driver::preludeSourceFingerprint());
    return *v;
}

// K2's composition, likewise pure. The TAG makes a link key structurally
// distinct from an object key even if every other byte coincided.
inline std::string composeLinkKey(const std::array<uint8_t, 32>& objectHash,
                                  const std::array<uint8_t, 32>& runtimeId,
                                  const std::string& recipe,
                                  const std::array<uint8_t, 32>& toolchainId) {
    llvm::SHA256 h;
    static const char kTag[] = "k3l;";
    h.update(llvm::StringRef(kTag, sizeof(kTag) - 1));
    h.update(llvm::ArrayRef<uint8_t>(objectHash.data(), objectHash.size()));
    h.update(llvm::ArrayRef<uint8_t>(runtimeId.data(), runtimeId.size()));
    h.update(llvm::StringRef(recipe.data(), recipe.size()));
    // per #3006: the link runs `clang …` (Driver::linkExecutableObject shells
    // out via std::system), so the linked bytes depend on the external clang/ld
    // and the system libs they resolve. The recipe signature spells the flags
    // verbatim but NOT clang's/ld's VERSION, so two toolchain builds producing
    // the same recipe string could otherwise collide. Folding the toolchain
    // identity in closes that.
    h.update(llvm::ArrayRef<uint8_t>(toolchainId.data(), toolchainId.size()));
    return toHex(h.final());
}

// K1: the OBJECT key — compiler identity ‖ codegen mode ‖ stdlib prelude ‖
// source: every input the emitted object depends on. Deliberately free of any
// runtime component (see above). "" when the split is unusable.
inline std::string computeObjectKey(std::string_view source,
                                    const codegen::CodegenOptions& opts) {
    const BuildIdentity& id = buildIdentity();
    if (!id.usable || !id.splitUsable) return {};
    return composeObjectKey(id.compiler, codegenModeString(opts),
                            preludeFingerprint(), source, id.toolchain);
}

// K2: the LINK key — the object's CONTENT hash ‖ runtime identity ‖ recipe.
// The object hash (not its cache key) is what makes this sound across a
// compiler change: two different compilers that emit the same object bytes must
// link to the same executable, and that is exactly what a hit asserts.
// `sanitizeAddress` reaches the key only through the recipe, which spells the
// `-fsanitize=address -g` flags out verbatim — so an ASan and a plain product
// can never collide even before considering that their objects differ too.
inline std::string computeLinkKey(const std::array<uint8_t, 32>& objectHash,
                                  const codegen::CodegenOptions& opts) {
    const BuildIdentity& id = buildIdentity();
    if (!id.usable || !id.splitUsable) return {};
    return composeLinkKey(objectHash, id.runtime,
                          linkRecipeSignature(opts.sanitizeAddress), id.toolchain);
}

// Per-process hit/miss counters (a hit is served from the cache; a miss ran the
// real compiler — including reject/crash/timeout, which are never cacheable).
struct CacheStats {
    std::atomic<std::uint64_t> hits{0};      // executable served (KE or K2)
    std::atomic<std::uint64_t> misses{0};    // the real linker ran
    std::atomic<std::uint64_t> exeKeyHits{0};  // KE: whole build skipped
    std::atomic<std::uint64_t> objHits{0};     // K1: front-end skipped
    std::atomic<std::uint64_t> linkHits{0};    // K2: link skipped after a compile
    // per #3014: a silently-failing store is the #3003 defect class (each of 17
    // PIDs stored ~1,700-2,500 entries then failed every store until exit, with
    // NO telemetry of it). Counting failures + the last errno makes that class
    // visible in the stats sink. storeFsyncs (#3009) proves the durability fsync
    // actually ran on the install path — the regression test pins the mechanism.
    std::atomic<std::uint64_t> storeFailures{0};
    std::atomic<std::uint64_t> storeFsyncs{0};
    // per #3669: renames that SKIPPED the #3009 durability fsync because
    // BEZEL_AOT_STORE_FSYNC=0 was in force. A no-fsync storeToCache bumps this
    // TWICE (the entry's own rename and its size-sidecar's rename), so it is a
    // "how much durability was traded away" signal, not a store count.
    std::atomic<std::uint64_t> storeFsyncSkipped{0};
    // per #3669: cache entries REJECTED on read because their size sidecar
    // disagreed with the entry's actual size (a torn/short entry). Deliberately
    // NOT called `verify_misses` on the [aot-cache] line: gate.sh:1086 sums the
    // shard log with an UNANCHORED `grep -aoE 'misses=[0-9]+'`, so a field
    // spelled `verify_misses=` would be silently added into the gate's reported
    // AOT miss total. `verify_rejects=` cannot collide with any of the three
    // fields the gate greps (hits= / misses= / store_failures=).
    std::atomic<std::uint64_t> verifyRejects{0};
    std::atomic<int> lastStoreErrno{0};
};
inline CacheStats& cacheStats() {
    // Leaked on purpose: the BEZEL_AOT_CACHE_STATS atexit hook reads these
    // counters at process teardown, so they must outlive static destruction.
    static CacheStats* s = new CacheStats();
    return *s;
}

inline bool cacheStatsEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_CACHE_STATS");
        return e && std::string(e) == "1";
    }();
    return v;
}

// per #3014: the AOT cache stats' OWN sink. When set, the [aot-cache] line goes
// to this file atomically (see emitCacheStatsNow) INSTEAD OF stderr. Read fresh
// (not static-cached) so a test can setenv and observe it in a forked child; it
// is consulted only at stats-emit time (once per process), so the cost is
// negligible. This is the structural #3012 fix: with the env set the [aot-cache]
// line NEVER touches the shard's stderr/summary stream, so the atexit hook
// cannot race the shard's own summary printer mid-line and corrupt the
// "N passed, M failed" parse (#3012).
inline std::string cacheStatsFile() {
    const char* e = std::getenv("BEZEL_AOT_CACHE_STATS_FILE");
    return (e && *e) ? std::string(e) : std::string{};
}

// per #3669: is the #3009 store fsync in force? DEFAULT YES — the env unset (or
// set to anything but "0") gives exactly the #3009 behaviour, byte for byte, so
// no normal run changes. `BEZEL_AOT_STORE_FSYNC=0` trades the store's
// durability for its cost (~21k data fsyncs + as many directory fsyncs per ASan
// gate pass, measured 2026-09-02) and is compensated by verify-on-read below.
//
// Read FRESH on every call — deliberately NOT a memoised static — so a test can
// setenv/unsetenv and exercise both modes inside one process. The read is a
// getenv per cache-store rename (three per cold test), which is noise beside the
// rename it guards.
inline bool storeFsyncEnabled() {
    const char* e = std::getenv("BEZEL_AOT_STORE_FSYNC");
    return !(e && std::string(e) == "0");
}

// per #3009: fsync the temp file's DATA, then atomically rename it into place,
// then best-effort fsync the target directory so the rename itself is durable.
// Returns the rename's error_code (empty on success); on rename failure the
// temp is removed. The data fsync is the load-bearing half: without it, a crash
// between the byte copy and the rename could leave a partial/truncated cache
// entry that a later lookup mistakes for whole (#3009 — latent: zero truncated
// entries in 53,703, but a cheap durability fix). Shared by storeToCache (cache
// install) and the BEZEL_AOT_CACHE_STATS_FILE sink (#3014). storeFsyncs is the
// mechanism pin the #3009 regression test asserts against.
//
// per #3669: `doFsync` selects the mode and DEFAULTS to storeFsyncEnabled(), so
// every existing two-argument call site keeps the #3009 behaviour unchanged. The
// default argument is evaluated at each call (not once), which is what lets a
// test flip the env mid-process. It is spelled `doFsync`, not `fsync`, because
// the body calls ::fsync and a parameter of that name shadows it.
inline std::error_code durableRename(const std::string& tmp,
                                     const std::string& dst,
                                     bool doFsync = storeFsyncEnabled()) {
    if (doFsync) {
        if (int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
            while (::fsync(fd) != 0 && errno == EINTR) {}
            ::close(fd);
            cacheStats().storeFsyncs.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        cacheStats().storeFsyncSkipped.fetch_add(1, std::memory_order_relaxed);
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);  // atomic install (same filesystem)
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return ec; }
    // Best-effort directory fsync (some filesystems refuse; harmless — the data
    // fsync above is what prevents a truncated entry). Skipped with the data
    // fsync when !doFsync: fsyncing the directory alone would make the rename
    // durable over data blocks that may not be, which is the #3009 hazard
    // itself, and would keep most of the cost.
    if (doFsync) {
        if (int dfd = ::open(fs::path(dst).parent_path().string().c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC); dfd >= 0) {
            while (::fsync(dfd) != 0 && errno == EINTR) {}
            ::close(dfd);
        }
    }
    return {};
}

// per #3669: the size sidecar for a cache entry. `<entry>.sz` holds the decimal
// byte count the entry had when it was stored; installFromCache treats a
// disagreement as a torn entry (a MISS) instead of running it. This is the
// verification that replaces the durability BEZEL_AOT_STORE_FSYNC=0 gives up.
//
// The `.sz` suffix cannot collide with an entry name: entries are 64-hex keys
// (exeEntryPath/objEntryPath/linkEntryPath below), and a sidecar's own basename
// therefore never parses as a key.
inline std::string cacheSidecarPath(const std::string& entry) {
    return entry + ".sz";
}

// per #3669: record `src`'s byte size in `<entry>.sz`, atomically (temp in the
// SAME directory, then rename) and WITHOUT an fsync — the sidecar is a
// verification aid, not a durability claim, so a sidecar lost to the same power
// cut that tore the entry simply degrades to the pre-#3669 "trust the entry"
// behaviour rather than making anything worse.
//
// Best-effort throughout: on any failure no sidecar is left, and an entry with
// no sidecar is trusted exactly as today. The temp is named with the `.tmp.`
// prefix so maybeGcCache's orphan reaper (#3003) covers it.
inline void writeSizeSidecar(const std::string& src, const std::string& entry) {
    std::error_code ec;
    const std::uintmax_t sz = fs::file_size(src, ec);
    if (ec) return;
    static std::atomic<std::uint64_t> ctr{0};
    const std::string dir = fs::path(entry).parent_path().string();
    const std::string tmp =
        dir + "/.tmp.sz." + std::to_string(::getpid()) + "." +
        std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { std::error_code e2; fs::remove(tmp, e2); return; }
        f << sz;
        f.close();
        if (!f) { std::error_code e2; fs::remove(tmp, e2); return; }
    }
    durableRename(tmp, cacheSidecarPath(entry), /*doFsync=*/false);
}

// per #3669: read the size recorded in `sidecarPath`. nullopt when the sidecar
// is absent, unreadable, empty or malformed — every one of which means "no
// verifiable claim about this entry", i.e. trust it as before #3669. Only a
// sidecar that parses cleanly can reject an entry.
inline std::optional<std::uintmax_t> readSizeSidecar(
    const std::string& sidecarPath) {
    std::ifstream f(sidecarPath, std::ios::binary);
    if (!f) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.empty()) return std::nullopt;
    // std::from_chars, not stoull: no exceptions (LLVM's flags can carry
    // -fno-exceptions) and it rejects overflow by error code rather than throw.
    std::uintmax_t v = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc{} || res.ptr != last) return std::nullopt;
    return v;
}

// per #3014: atomically write `content` to `path` (temp in the same dir, then
// durableRename — fsync + atomic rename + dir fsync, reusing the #3009 helper).
// A concurrent reader never sees a partial line. Best-effort: on any failure no
// file is left, which the reader treats as "no stats for this shard" rather than
// a truncated line.
inline void atomicWriteStats(const std::string& path,
                             const std::string& content) {
    std::error_code ec;
    const std::string dir = fs::path(path).parent_path().string();
    if (!dir.empty()) fs::create_directories(dir, ec);
    static std::atomic<std::uint64_t> ctr{0};
    const std::string tmp =
        path + ".tmp." + std::to_string(::getpid()) + "." +
        std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { std::error_code e2; fs::remove(tmp, e2); return; }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) { std::error_code e2; fs::remove(tmp, e2); return; }
        f.close();
        if (!f) { std::error_code e2; fs::remove(tmp, e2); return; }
    }
    // per #3669: PIN the fsync explicitly rather than inheriting
    // durableRename's new default. This sink is not a cache entry: it is written
    // once per process at exit, there is no verify-on-read for it (no size
    // sidecar, and its reader is a grep, not an executable), and gate telemetry
    // reads it after the run. Letting BEZEL_AOT_STORE_FSYNC=0 reach it would
    // have silently dropped the #3014 sink's durability too — a widening beyond
    // what #3669 asks for, at zero measurable saving (one fsync per process
    // against ~21k per gate pass).
    durableRename(tmp, path, /*doFsync=*/true);  // fsync + atomic rename + dir fsync
}

// per #3014: emit the [aot-cache] stats line NOW to the selected sink:
//   1. BEZEL_AOT_CACHE_STATS_FILE set -> atomic file write (the #3012 fix: keeps
//      the line OFF the shard's stderr/summary stream).
//   2. else BEZEL_AOT_CACHE_STATS=1 -> legacy stderr print (byte-identical to
//      the pre-#3014 atexit hook when the new env is unset).
//   3. else -> nothing.
// Factored out of the atexit hook so the sink selection is observable in a
// forked child without depending on process teardown (the regression test calls
// this directly with stderr captured to prove no [aot-cache] lands on it).
inline void emitCacheStatsNow() {
    auto& s = cacheStats();
    const unsigned long long h = s.hits.load(std::memory_order_relaxed);
    const unsigned long long m = s.misses.load(std::memory_order_relaxed);
    const unsigned long long eh = s.exeKeyHits.load(std::memory_order_relaxed);
    const unsigned long long oh = s.objHits.load(std::memory_order_relaxed);
    const unsigned long long lh = s.linkHits.load(std::memory_order_relaxed);
    const unsigned long long sf = s.storeFailures.load(std::memory_order_relaxed);
    const unsigned long long fy = s.storeFsyncs.load(std::memory_order_relaxed);
    const unsigned long long fsk =
        s.storeFsyncSkipped.load(std::memory_order_relaxed);   // #3669
    const unsigned long long vr =
        s.verifyRejects.load(std::memory_order_relaxed);        // #3669
    const int lse = s.lastStoreErrno.load(std::memory_order_relaxed);
    char buf[640];
    const int n = std::snprintf(buf, sizeof(buf),
        "[aot-cache] hits=%llu misses=%llu (%.0f%% hit) "
        "exe-key-hits=%llu obj-hits=%llu link-hits=%llu "
        "store_failures=%llu store_fsyncs=%llu store_errno=%d "
        // per #3669: APPENDED, never reordered — gate.sh:1084-1089 greps this
        // line field-by-field (`hits=` anchored on the [aot-cache] prefix,
        // `misses=`, `store_failures=`), so a new field it does not name is
        // invisible to it. Neither name contains any of those three tokens.
        "store_fsync_skipped=%llu verify_rejects=%llu dir=%s\n",
        h, m, (h + m) ? 100.0 * h / (h + m) : 0.0, eh, oh, lh, sf, fy, lse,
        fsk, vr, cacheDir().c_str());
    if (n < 0) return;
    // Clamp to the bytes snprintf actually wrote (it returns the would-be size,
    // which can exceed the buffer on truncation).
    const std::size_t len = static_cast<std::size_t>(
        n < static_cast<int>(sizeof(buf)) ? n
                                          : static_cast<int>(sizeof(buf)) - 1);
    const std::string statsFile = cacheStatsFile();
    if (!statsFile.empty()) {
        atomicWriteStats(statsFile, std::string(buf, len));
        return;
    }
    if (cacheStatsEnabled()) std::fwrite(buf, 1, len, stderr);
}

// Register (once) an atexit hook that emits the stats line at shard exit when a
// sink is opted into (BEZEL_AOT_CACHE_STATS_FILE or BEZEL_AOT_CACHE_STATS=1).
// Default off → normal runs print nothing. The hook calls emitCacheStatsNow, so
// the file sink (#3014) keeps the line OFF stderr — the #3012 race is fixed.
inline void ensureStatsAtExit() {
    static const bool once = [] {
        // Whether to register is decided once at first compile; the file env is
        // RE-READ by emitCacheStatsNow at exit, so a late setenv still routes to
        // the file. Register if EITHER sink could fire.
        if (cacheStatsEnabled() || !cacheStatsFile().empty()) {
            std::atexit(emitCacheStatsNow);
        }
        return true;
    }();
    (void)once;
}

// Install a cached executable into the test's workdir at `exePath`: a hardlink
// (no byte copy — scratchBase() is derived from cacheDir() precisely so the two
// share a filesystem) or, if they somehow do not, a copy. Returns false on
// failure, in which case the caller falls back to a real compile.
//
// The copy fallback is the SLOW path, not a neutral alternative: when it was
// silently taken (the workdir on /var/tmp, the cache symlinked to another
// spindle) it cost 34% of an ASan slice's wall — see scratchBase().
inline bool installFromCache(const std::string& cachePath,
                             const std::string& exePath) {
    std::error_code ec;
    // per #3669: VERIFY-ON-READ, before the entry is linked anywhere. When the
    // entry carries a size sidecar — written only by a store that skipped the
    // #3009 durability fsync — a size disagreement means a torn/short entry.
    // Treat it as a MISS: evict entry AND sidecar and return false, which every
    // caller already handles by running the real compile (cachedCompile's three
    // lookups all fall through on false). That preserves exactly the property
    // #3009 bought — a hit never runs a truncated executable — by verification
    // instead of durability.
    //
    // An entry WITHOUT a parseable sidecar is trusted exactly as before #3669:
    // it was stored WITH the fsync, so there is nothing to verify against.
    //
    // The check is deliberately NOT conditioned on THIS reader's own
    // BEZEL_AOT_STORE_FSYNC. The store is shared between the gate (which opts
    // in) and lane/agent_check runs (which do not), so an entry written without
    // the fsync must be verified by whoever reads it, not only by a reader in
    // the same mode. The price on the default path is one open() that ENOENTs
    // per install — the ONLY syscall #3669 adds to a default-mode run, and it
    // can only ever turn a hit into a miss (a real compile), never a wrong
    // product. A vanished entry is left to the hardlink below to fail on, so
    // that race is not miscounted as a verification reject.
    const std::string sidecar = cacheSidecarPath(cachePath);
    if (const std::optional<std::uintmax_t> want = readSizeSidecar(sidecar)) {
        std::error_code e1;
        const std::uintmax_t got = fs::file_size(cachePath, e1);
        if (!e1 && got != *want) {
            cacheStats().verifyRejects.fetch_add(1, std::memory_order_relaxed);
            std::error_code e2;
            fs::remove(cachePath, e2);
            fs::remove(sidecar, e2);
            return false;
        }
    }
    fs::create_hard_link(cachePath, exePath, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(cachePath, exePath,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }
    // A hardlink already shares the cached inode's 0755; a fresh copy may not,
    // so set the exec bits explicitly either way.
    fs::permissions(exePath, kExePerms, fs::perm_options::replace, ec);
    return fs::exists(exePath, ec);
}

// Atomically install the freshly-linked `exePath` into the cache at `cachePath`
// (write a temp in the SAME directory, then rename). Best-effort: any failure is
// swallowed — the next run simply recomputes. Concurrent shards racing on the
// same key rename benignly (identical content; the last writer wins).
inline void storeToCache(const std::string& exePath,
                         const std::string& cachePath) {
    static std::atomic<std::uint64_t> ctr{0};
    std::error_code ec;
    const std::string sub = fs::path(cachePath).parent_path().string();
    const std::string tmp =
        sub + "/.tmp." + std::to_string(::getpid()) + "." +
        std::to_string(ctr.fetch_add(1, std::memory_order_relaxed));
    // Prefer a hardlink of the just-linked exe (no copy). WorkDir later unlinks
    // the workdir's link; the cache's link survives.
    fs::create_hard_link(exePath, tmp, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(exePath, tmp, fs::copy_options::overwrite_existing, ec);
        // fix #3003: REMOVE THE TEMP ON FAILURE. This was a bare `return`, so
        // every failed store leaked its .tmp file. Measured 2026-08-08: 9,160
        // orphans in the store, 9,158 of them zero-byte — 7,661 inside a single
        // hour, which was one ASan gate pass. Each of 17 PIDs produced a
        // CONTIGUOUS run of temp counters, i.e. each process stored ~1,700-2,500
        // entries successfully and then failed EVERY store until it exited,
        // silently. Combined with the GC skipping .tmp names below, those bytes
        // were invisible to the cap.
        if (ec) {
            // per #3014: count every store failure and capture the errno. The
            // #3003 orphan storm was exactly this path failing silently for
            // thousands of stores per shard; the counter makes it visible in
            // the stats sink instead of detectable only by du.
            cacheStats().storeFailures.fetch_add(1, std::memory_order_relaxed);
            cacheStats().lastStoreErrno.store(
                static_cast<int>(ec.value()), std::memory_order_relaxed);
            std::error_code e2; fs::remove(tmp, e2);
            return;
        }
    }
    fs::permissions(tmp, kExePerms, fs::perm_options::replace, ec);
    ec.clear();
    // per #3009: durableRename fsyncs the temp's data before the atomic rename
    // (and best-effort fsyncs the cache dir) so a crash between the copy and the
    // rename-in-cache-dir cannot leave a partial/truncated entry serving a
    // future hit.
    //
    // per #3669: with BEZEL_AOT_STORE_FSYNC=0 that durability is traded for its
    // cost and replaced by verify-on-read, so record the entry's size in a
    // sidecar FIRST. Order matters: a sidecar without its entry is inert (the
    // lookup misses on the absent entry), whereas an entry without its sidecar
    // is merely trusted — the pre-#3669 state. The mode is sampled ONCE here and
    // handed to durableRename so the sidecar and the rename cannot disagree if a
    // concurrent test flips the env between the two.
    //
    // The sidecar is written ONLY in no-fsync mode, on purpose: the default path
    // must stay byte-for-byte today's syscall sequence (this header is compiled
    // into every test TU and the store runs on every cold test). The cost is
    // that an entry stored under the fsync is not size-verified — which is the
    // pre-#3669 contract, and sound, because that store WAS durable.
    const bool doFsync = storeFsyncEnabled();
    if (!doFsync) writeSizeSidecar(tmp, cachePath);
    const std::error_code rec = durableRename(tmp, cachePath, doFsync);
    if (rec) {
        // per #3014: the rename itself failed — count it too (durableRename has
        // already removed the temp).
        cacheStats().storeFailures.fetch_add(1, std::memory_order_relaxed);
        cacheStats().lastStoreErrno.store(
            static_cast<int>(rec.value()), std::memory_order_relaxed);
    }
}

// Orphan-temp age floor for the GC below (#3003). The compile+link deadline is
// 120s, so an hour is far past any in-flight store and cannot race a live one.
inline constexpr auto kTempOrphanAge = std::chrono::hours(1);

// Size-bounded GC, run ONCE per process under a non-blocking flock so only one
// shard sweeps (losers skip; GC is a coarse safety valve, not per-run-critical).
// If the store exceeds the cap, delete oldest-mtime entries down to 3/4 of it.
inline void maybeGcCache() {
    static const bool once = [] {
        if (!cacheEnabled()) return true;
        const std::string dir = cacheDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string lockPath = dir + "/.gc.lock";
        int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return true;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return true; }

        // Cap: BEZEL_AOT_CACHE_MAX_GB, default 64 GiB. The former 5 GiB cap
        // predates the k2 identity (#2100): under the k1 key entries rarely
        // outlived a relink, so the cap only bounded garbage. Under k2 entries
        // are DESIGNED to persist across gates, and one full-suite ASan pass
        // alone stores ~30 GiB — with a 5 GiB cap every next gate evicted the
        // previous one wholesale (measured 2026-07-23: a should-be-warm --full
        // re-gate ran cold-by-eviction under the mid-gate unlink storm and
        // ground to the 900s shard ceiling). Target 3/4 of cap, not half: a
        // sweep trims the cold tail without discarding the working set.
        std::uintmax_t capGb = 64;
        if (const char* e = std::getenv("BEZEL_AOT_CACHE_MAX_GB")) {
            char* end = nullptr;
            unsigned long v = std::strtoul(e, &end, 10);
            if (end && *end == '\0' && v > 0) capGb = v;
        }
        const std::uintmax_t cap = static_cast<std::uintmax_t>(capGb) << 30;
        const std::uintmax_t target = cap / 4 * 3;
        struct Ent { std::string path; std::uintmax_t size; fs::file_time_type mt; };
        std::vector<Ent> ents;
        std::uintmax_t total = 0;
        for (fs::recursive_directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) continue;
            const std::string name = it->path().filename().string();
            if (name == ".gc.lock") continue;
            std::uintmax_t sz = fs::file_size(it->path(), e2); if (e2) continue;
            fs::file_time_type mt = fs::last_write_time(it->path(), e2);
            if (e2) continue;
            // fix #3003: COUNT AND REAP ORPHANED TEMPS.
            //
            // `.tmp.` names were skipped for BOTH counting and deletion, so the
            // cap bounded only the countable subset — and the uncountable subset
            // is precisely the one that grows when stores start failing. That
            // makes BEZEL_AOT_CACHE_MAX_GB not an enforceable ceiling at all.
            //
            // A temp older than kTempOrphanAge cannot belong to a live store:
            // the whole compile+link has a 120s deadline, so an hour is orders
            // of magnitude past any in-flight write, and a concurrent storer's
            // fresh temp is never touched.
            if (name.rfind(".tmp.", 0) == 0) {
                const auto age = fs::file_time_type::clock::now() - mt;
                if (age > kTempOrphanAge) {
                    std::error_code e3;
                    fs::remove(it->path(), e3);   // reclaim; not added to `ents`
                    continue;
                }
                total += sz;   // young temp: counts against the cap, not reaped
                continue;
            }
            // per #3669: `<key>.sz` size sidecars are ordinary files here, so
            // they are counted and are eligible for eviction like any entry.
            // Two consequences, deliberately not special-cased:
            //   * the file COUNT roughly doubles for entries stored in no-fsync
            //     mode (the BYTES do not — a sidecar is <8 bytes, so the GB cap
            //     this loop enforces is unaffected). That inflates the cold/warm
            //     classifier's `cache_n` (scripts/gate/cold.py count_cache, a
            //     `find -L -type f` count) by up to 2x, so its
            //     COLD_CACHE_THRESHOLD of 2000 files stops meaning 2000 entries
            //     and starts meaning ~1000. This is NOT fail-safe by itself —
            //     it biases the verdict toward WARM, whose ceiling (900s) is the
            //     TIGHTER one. It is bounded, though: the misjudgement band is
            //     only 1000-2000 real entries, far below a populated gate store
            //     (102,299 measured in #3000) and far above a fresh one, and the
            //     count is one of THREE OR'd cold signals — a genuinely cold
            //     store almost always also trips `identity_changed` or
            //     `marker_changed`, which force COLD on their own.
            //   * a sidecar may be reaped before its entry, which degrades that
            //     entry to the pre-#3669 "trusted, unverified" state — i.e. to
            //     exactly the fsync-mode contract, never to something weaker.
            total += sz;
            ents.push_back({it->path().string(), sz, mt});
        }
        if (total > cap) {
            std::sort(ents.begin(), ents.end(),
                      [](const Ent& a, const Ent& b) { return a.mt < b.mt; });
            for (const Ent& e : ents) {
                if (total <= target) break;
                std::error_code e2;
                if (fs::remove(e.path, e2) && !e2) total -= e.size;
            }
        }
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return true;
    }();
    (void)once;
}

// Store paths. Whole-executable (KE) entries keep the original
// <cacheDir>/<first2>/<key> layout so everything the pre-split harness stored is
// still found; the two new kinds get their own prefix:
//     <cacheDir>/o/<first2>/<key>   objects       (K1)
//     <cacheDir>/l/<first2>/<key>   link products (K2)
// The prefixes are one character and the KE subdirectories are two hex digits,
// so the namespaces cannot collide. Segregating them is not required for
// correctness (the keys are differently tagged) but it makes the store
// self-describing: each kind can be counted, aged, or dropped on its own —
// which is exactly what a "what would a runtime-only landing cost?" experiment
// needs to do, and what makes the cache auditable rather than opaque.
inline std::string exeEntryPath(const std::string& key) {
    return cacheDir() + "/" + key.substr(0, 2) + "/" + key;
}
inline std::string objEntryPath(const std::string& key) {
    return cacheDir() + "/o/" + key.substr(0, 2) + "/" + key;
}
inline std::string linkEntryPath(const std::string& key) {
    return cacheDir() + "/l/" + key.substr(0, 2) + "/" + key;
}

// Compile `srcPath` to `exePath`, serving cached build products wherever the
// inputs are unchanged. Three lookups, cheapest first (see the "k3" comment on
// computeObjectKey/computeLinkKey for the keys and why each is sound):
//
//   1. KE — the whole executable, keyed exactly as before the split. On a
//      fully-warm run this hits and the function is byte-for-byte the old fast
//      path: one hash, one hardlink, no object I/O.
//   2. K1 — the object. A hit means the front-end need not run (the compiler
//      and the source are unchanged even though something else moved).
//   3. K2 — the executable, keyed on the object's CONTENT. A hit means the link
//      need not run (this exact object was already linked against this exact
//      runtime with this exact recipe).
//
// A hit at 1 or 3 runs NO link (and holds no #1780 token for one). Whenever the
// two-level path produces an executable it is ALSO stored under KE, so the next
// run of the same tree collapses back to the single cheap lookup.
//
// On a compile REJECT/CRASH/TIMEOUT nothing is cached (the real diagnostics stay
// live). The returned CompileStatus is indistinguishable from forkedCompile's —
// a hit yields {Ok, "", 0} with exePath populated.
inline CompileStatus cachedCompile(std::string_view source,
                                   const std::string& srcPath,
                                   const std::string& exePath,
                                   const codegen::CodegenOptions& opts,
                                   const std::string& ccErrPath) {
    // The crash-isolation regression test (aot_crash_isolation_test.cpp) arms
    // BEZEL_AOT_TEST_CRASH_HOOK to force the forked compile CHILD to SIGSEGV and
    // asserts the isolated ran=false. That fault is injected INSIDE the real
    // forked compile, so a cache HIT — which skips the compile entirely — would
    // bypass it and the test would observe a successful run (a cache-on vs
    // cache-off divergence). When the hook is armed, always take the real
    // compile path. The hook is a test-only affordance, never set in normal runs.
    //
    // BEZEL_DUMP_IR is treated the same way: it asks the compiler to PRINT the
    // module it emits, which a served product cannot do. A debugging affordance
    // that silently produced no output would be worse than useless.
    if (std::getenv("BEZEL_AOT_TEST_CRASH_HOOK") || std::getenv("BEZEL_DUMP_IR"))
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    if (!cacheEnabled())
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    ensureStatsAtExit();
    const std::string exeKey = computeCacheKey(source, opts);
    if (exeKey.empty())  // no sound compiler identity → never risk a stale hit
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    maybeGcCache();  // once per process (guarded)

    const std::string exePathInCache = exeEntryPath(exeKey);

    // Best-effort LRU touch so a hot entry survives the size GC.
    auto touch = [](const std::string& p) {
        std::error_code e;
        fs::last_write_time(p, fs::file_time_type::clock::now(), e);
    };
    auto installExe = [&](const std::string& from) {
        if (!installFromCache(from, exePath)) return false;
        cacheStats().hits.fetch_add(1, std::memory_order_relaxed);
        touch(from);
        return true;
    };

    std::error_code ec;
    // ---- 1. whole-executable key (the unchanged fast path) ------------------
    if (fs::exists(exePathInCache, ec) && !ec && installExe(exePathInCache)) {
        cacheStats().exeKeyHits.fetch_add(1, std::memory_order_relaxed);
        return {CompileOutcome::Ok, {}, 0};
    }
    // Install failed (disk error / vanished entry) → fall through rather than
    // fail the test.

    const std::string objKey = computeObjectKey(source, opts);
    if (objKey.empty()) {
        // The split is unusable in this configuration (no known runtime
        // archive): fall back to the pre-split single-key behaviour exactly.
        CompileStatus cc = forkedCompile(srcPath, exePath, opts, ccErrPath);
        cacheStats().misses.fetch_add(1, std::memory_order_relaxed);
        if (cc.outcome == CompileOutcome::Ok) {
            fs::create_directories(fs::path(exePathInCache).parent_path(), ec);
            storeToCache(exePath, exePathInCache);
        }
        return cc;
    }

    // ---- 2. object key -----------------------------------------------------
    const std::string objPathInCache = objEntryPath(objKey);
    const std::string objPath = exePath + ".o";   // the driver's own naming
    bool haveObject = false;
    if (fs::exists(objPathInCache, ec) && !ec) {
        // Hardlink the cached object into the workdir BEFORE using it: that
        // pins the inode, so a concurrent GC unlinking the entry between here
        // and the link cannot turn a cache hit into a spurious test failure.
        if (installFromCache(objPathInCache, objPath)) {
            haveObject = true;
            touch(objPathInCache);
        }
    }
    if (!haveObject) {
        CompileStatus cc = forkedCompileObject(srcPath, objPath, opts, ccErrPath);
        if (cc.outcome != CompileOutcome::Ok) {
            cacheStats().misses.fetch_add(1, std::memory_order_relaxed);
            return cc;   // reject / crash / timeout: never cached
        }
        fs::create_directories(fs::path(objPathInCache).parent_path(), ec);
        storeToCache(objPath, objPathInCache);   // best-effort atomic install
    } else {
        cacheStats().objHits.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- 3. link key, over the object's CONTENT ----------------------------
    std::array<uint8_t, 32> objHash{};
    const std::string linkKey =
        sha256File(objPath, objHash) ? computeLinkKey(objHash, opts)
                                     : std::string{};
    std::string linkPathInCache;
    if (!linkKey.empty()) {
        linkPathInCache = linkEntryPath(linkKey);
        if (fs::exists(linkPathInCache, ec) && !ec &&
            installExe(linkPathInCache)) {
            cacheStats().linkHits.fetch_add(1, std::memory_order_relaxed);
            // Also record it under the whole-executable key so the next run of
            // this tree takes lookup 1 and never touches the object at all.
            fs::create_directories(fs::path(exePathInCache).parent_path(), ec);
            storeToCache(exePath, exePathInCache);
            std::error_code e2;
            fs::remove(objPath, e2);
            return {CompileOutcome::Ok, {}, 0};
        }
    }

    cacheStats().misses.fetch_add(1, std::memory_order_relaxed);
    CompileStatus cc = forkedLink(objPath, exePath, opts, ccErrPath);
    if (cc.outcome == CompileOutcome::Ok) {
        if (!linkKey.empty()) {
            fs::create_directories(fs::path(linkPathInCache).parent_path(), ec);
            storeToCache(exePath, linkPathInCache);
        }
        fs::create_directories(fs::path(exePathInCache).parent_path(), ec);
        storeToCache(exePath, exePathInCache);
    }
    std::error_code e2;
    fs::remove(objPath, e2);   // the driver removes its own .o; this is ours
    return cc;
}

// Write `source` to `srcPath`. Returns false on write failure (disk full etc.).
// fix #3007: `why` (optional) receives the errno so the `[ INFRA ]` marker can
// say "No space left on device" rather than only naming the path.
inline bool writeSource(const std::string& srcPath, std::string_view source,
                        std::string* why) {
    if (why) why->clear();
    errno = 0;
    std::ofstream os(srcPath, std::ios::binary);
    os.write(source.data(), static_cast<std::streamsize>(source.size()));
    os.flush();
    if (os) return true;   // qr1 N4: report a failed write as infra
    if (why) {
        const int e = errno;
        *why = "failed to write the program source to " + srcPath +
               (e ? std::string(" (errno=") + std::to_string(e) + ": " +
                        std::strerror(e) + ")"
                  : std::string(" (stream failed; errno not set)"));
    }
    return false;
}

// Signature-preserving overload (no external call sites today; kept so the
// two-argument spelling in any in-flight branch still compiles).
inline bool writeSource(const std::string& srcPath, std::string_view source) {
    return writeSource(srcPath, source, nullptr);
}

// Stamp an infrastructure failure onto a result AND fail the current test
// (#3007). The result is left maximally honest: it did not run, its exit code
// is the did-not-run sentinel, and the reason is recorded at Severity::NOTE —
// never Error — so `r.diagnostics.hasErrors()` stays FALSE and a legacy
// `BEZEL_EXPECT_TRUE(hasErrors())` cannot be satisfied by it.
inline JitResult& markInfra(JitResult& r, const char* where,
                            const std::string& reason) {
    noteInfraFailure(where, reason);
    r.ran = false;
    r.exitCode = -1;
    r.diagnostics.report(Severity::Note, SourceLocation{},
                         std::string("[INFRA] ") + (where ? where : "runAot") +
                             ": " + reason);
    return r;
}

}  // namespace aot_detail

// ---------------------------------------------------------------------------
// runAot — compile `source` to a native executable via the REAL driver
// (Driver::compileFile, forked for crash/hang isolation) and run it, capturing
// {ran, exitCode, stdout, diags}.
//
//   ran      : true iff it compiled, linked, and ran to normal termination.
//   exitCode : the process exit status (8-bit) when ran==true; -1 on a crash /
//              timeout (mirrors runJit's failWith).
//   stdoutText: the program's captured stdout (bounded to 256 KiB).
//   diagnostics: on a compile/link reject OR a compiler crash/timeout, one Error
//              diagnostic carrying the driver stderr (so `.hasErrors()` is true
//              and downstream wrappers that inspect diagnostics behave as under
//              runJit). A compiler crash is isolated (ran=false), never fatal.
// ---------------------------------------------------------------------------
inline JitResult runAot(std::string_view source,
                        const codegen::CodegenOptions& optsIn) {
    JitResult r;
    // Arm the exit tally the moment the AOT harness is used at all, so
    // `INFRA: 0 failure(s)` is a positive confirmation rather than silence.
    aot_detail::ensureInfraTallyAtExit();

    // #1822: in the whole-suite ASan sweep, force generated-code ASan on every
    // program (the runAotChecked recipe, suite-wide). A local copy keeps the
    // public signature and every call site unchanged.
    codegen::CodegenOptions opts = optsIn;
    if (aot_detail::forceSanitizeAddress()) opts.sanitizeAddress = true;

    std::string infraWhy;
    aot_detail::WorkDir wd{aot_detail::makeWorkDir(&infraWhy)};
    if (wd.path.empty())
        return aot_detail::markInfra(r, "runAot", infraWhy);
    namespace fs = aot_detail::fs;
    const std::string srcPath = (fs::path(wd.path) / "prog.bzl").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string ccErr   = (fs::path(wd.path) / "cc.err").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string errPath = (fs::path(wd.path) / "run.err").string();

    if (!aot_detail::writeSource(srcPath, source, &infraWhy))
        return aot_detail::markInfra(r, "runAot", infraWhy);

    // --- Compile + link via the real driver, forked (crash/hang isolated),
    //     served from the content-addressed link-cache on an unchanged input. --
    aot_detail::CompileStatus cc =
        aot_detail::cachedCompile(source, srcPath, exePath, opts, ccErr);
    if (cc.outcome != aot_detail::CompileOutcome::Ok) {
        // fix #3007: CompileOutcome::Infra is NOT a verdict on the program —
        // it is a refused fork (isolation could not be guaranteed) or a driver
        // that claimed success and produced no executable. Reporting it as an
        // Error made it read as a compile reject; route it to the infra channel.
        if (cc.outcome == aot_detail::CompileOutcome::Infra)
            return aot_detail::markInfra(r, "runAot", cc.diag);
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             std::move(cc.diag));
        return r;  // ran stays false — AOT is the stricter oracle (see header)
    }

    // --- Run the executable, capturing stdout (stderr -> separate file). ------
    // #1822: under forced ASan (the sweep) run with LSan OFF (detect_leaks=0) and
    // report-not-abort (abort_on_error=0). LSan is deliberately off suite-wide —
    // whole-suite leak detection is too noisy: negative tests that exit via an
    // uncaught exception legitimately leave heap live at exit. LSan coverage
    // stays on the curated runAotChecked call-sites (leak-clean by construction).
    // abort_on_error=0 makes ASan PRINT the report and exit rather than SIGABRT,
    // so the report text is captured intact.
    bool timedOut = false;
    std::vector<std::pair<std::string, std::string>> runEnv;
    if (opts.sanitizeAddress)
        runEnv.push_back({"ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0"});
    int execErrno = 0;
    int status = aot_detail::spawnCaptured({exePath}, outPath, errPath,
                                           aot_detail::aotTimeoutSec(), timedOut,
                                           runEnv, &execErrno);
    const int spawnErrno = errno;   // only meaningful when status < 0
    // fix #3007: fork() for the program run failed. spawnCaptured's -1 is not a
    // wait status; read as one it makes WIFSIGNALED true and reported "program
    // terminated by signal 127" — a fabricated crash of a program that never
    // started.
    if (status < 0)
        return aot_detail::markInfra(
            r, "runAot",
            "fork() failed launching the compiled program (errno=" +
                std::to_string(spawnErrno) + ": " +
                std::strerror(spawnErrno) + ")");
    // fix #3007: execv() failed — the executable the link produced could not be
    // started (ENOEXEC / ENOMEM / EACCES / it vanished). The child exits 127,
    // which is a value a Bezel Main may legitimately return, so the CLOEXEC
    // errno pipe, not the exit code, is what makes this distinguishable.
    if (execErrno != 0)
        return aot_detail::markInfra(
            r, "runAot",
            "could not exec the linked program " + exePath + " (errno=" +
                std::to_string(execErrno) + ": " + std::strerror(execErrno) +
                ")");
    r.stdoutText = aot_detail::slurp(outPath);

    if (timedOut) {
        r.exitCode = -1;
        std::string runErr = aot_detail::slurp(errPath);
        r.diagnostics.report(
            Severity::Error, SourceLocation{},
            "runAot: program did not terminate within " +
                std::to_string(aot_detail::aotTimeoutSec()) +
                "s; killed (runaway / deadlock)" +
                (runErr.empty() ? "" : "\n--- program stderr ---\n" + runErr));
        return r;
    }
    // #1822: surface any ASan report the program printed to its stderr so the
    // whole-suite sweep detects a memory error in ANY program. A clean program
    // prints nothing here → normal forced-ASan runs are unaffected. Only active
    // under sanitizeAddress (the forced sweep, or an explicit caller).
    if (opts.sanitizeAddress) {
        std::string errText = aot_detail::slurp(errPath);
        if (aot_detail::hasSanitizerReport(errText)) {
            aot_detail::emitSanitizerReportToStderr(errText);
            r.exitCode = -1;
            r.diagnostics.report(
                Severity::Error, SourceLocation{},
                "runAot: AddressSanitizer fired in the test program\n" + errText);
            return r;
        }
    }
    if (WIFSIGNALED(status)) {
        r.exitCode = -1;  // mirror runJit's failWith
        std::string runErr = aot_detail::slurp(errPath);
        r.diagnostics.report(
            Severity::Error, SourceLocation{},
            "runAot: program terminated by signal " +
                std::to_string(WTERMSIG(status)) +
                (runErr.empty() ? "" : "\n--- program stderr ---\n" + runErr));
        return r;
    }
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.ran = WIFEXITED(status);
    return r;
}

// Convenience: default (release) codegen options — full parity with runJit.
inline JitResult runAot(std::string_view source) {
    return runAot(source, codegen::CodegenOptions{});
}

// ---------------------------------------------------------------------------
// runAotCheckedDisposal — the AOT counterpart of the JIT harness's
// runJitChecked (support/codegen_support.hpp). This is checked-disposal MODE
// (§28.2 v0.134 / issue #601), NOT the ASan/LSan memory net that runAotChecked
// provides — the two "checked" helpers are ORTHOGONAL and must not be confused:
//
//   * runAotChecked  -> opts.sanitizeAddress = true : instruments the GENERATED
//     code with AddressSanitizer and links -fsanitize=address, catching
//     use-after-free / out-of-bounds READs and leaks in emitted loads (WS2).
//   * runAotCheckedDisposal (here) -> opts.checkedDisposal = true : §28.2's
//     LANGUAGE-LEVEL rule that any instance-member access on a logically-
//     disposed instance raises DisposedAccessException (§31.4). Instance-method
//     entries and property/indexer/field accessors emit a disposed-flag guard,
//     and the heap free is quarantined so the flag check after disposal is
//     well-defined. This is exactly `bezelc --checked-disposal` (main.cpp) and
//     mirrors runJitChecked, which sets the identical bit (opts.checkedDisposal
//     = true) before running.
//
// It drives the SAME forked Driver::compileFile + subprocess-run path as runAot
// (via the runAot(source, opts) overload) — the shipping AOT oracle, stricter
// than the JIT harness — differing from plain runAot by exactly one bit,
// opts.checkedDisposal. A program that uses a disposed instance therefore raises
// DisposedAccessException (which a `catch DisposedAccessException` arm can
// observe, e.g. returning 1) here, whereas under plain runAot no guard is
// emitted and the access is undefined behaviour. The return type is JitResult,
// so `.ran` / `.exitCode` / `.diagnostics` call sites are untouched.
inline JitResult runAotCheckedDisposal(std::string_view source) {
    codegen::CodegenOptions opts;
    opts.checkedDisposal = true;  // §28.2 checked-disposal MODE (not ASan)
    return runAot(source, opts);
}

// ---------------------------------------------------------------------------
// runAotOut — the VALUE CHANNEL for observables that do not fit an 8-bit exit
// code. A process exit status is masked to 0..255 (WEXITSTATUS), so a program
// whose Main returns a value outside that range — a negative (IndexOf-not-found
// = -1, a negative mod result), or a large one (1000, 2024, 60007) — cannot
// round-trip through `runAot(...).exitCode` the way it did through the in-process
// JIT's raw 32-bit int (see the header's "TWO SEMANTIC DIFFERENCES", #1).
//
// The mechanical migration of such a test is: make the program PRINT its result
// with `Console.WriteLine(<expr>)` (stdlib §2.1 — real API, per-type numeric
// overloads since v0.165) and `return 0`, then assert on the captured stdout
// instead of the exit code. runAotOut is the one-call counterpart to that: it
// returns the program's stdout with a SINGLE trailing line-ending stripped, so
// the expected string is the bare "<value>" (NOT "<value>\n"). Console.WriteLine
// appends Console.NewLine — "\n" on Unix, "\r\n" on Windows — so baking the
// terminator into the expected string would be platform-dependent; stripping it
// here keeps every migrated test's expectation platform-independent. Returns the
// sentinel "<did-not-run>" if the program was rejected / crashed / timed out —
// mirroring the `run()` helpers' `r.ran ? … : -999` idiom on the string channel.
// A test that prints MULTIPLE lines (or needs the raw bytes) should use
// runAot(...).stdoutText directly rather than this single-value helper.
inline std::string runAotOut(std::string_view source) {
    auto r = runAot(source);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();  // CRLF
    }
    return out;
}

// ---------------------------------------------------------------------------
// runAotChecked — the per-program AddressSanitizer/LeakSanitizer memory net
// (migration plan §2 memory tier; WS2). The SAME forked Driver::compileFile as
// runAot but with `opts.sanitizeAddress = true`, so the GENERATED Bezel code is
// ASan-instrumented (qr1 S3: use-after-free / out-of-bounds READs in emitted
// loads are caught at the access, not merely the allocator-level leaks/double-
// frees the runtime interceptors net) and the executable is linked against the
// ASan runtime, then run under LeakSanitizer. A sanitizer error at exit (leak,
// double-free, use-after-free) FAILS: it surfaces as ran=false with the report
// in `diagnostics`, so a wrapper doing `r.ran ? r.exitCode : -999` flips to the
// failure sentinel. A clean program returns normally (ran=true, exitCode=
// program's, LSan-clean).
//
// qr1 S2: one pipeline with runAot (same forkedCompile, same §33.5 entry-point
// check) — they can't drift. qr1 M3: a real sanitizer failure is detected by the
// ASan/LSan REPORT TEXT, never a bare exit code, so a program legitimately
// returning 99 from Main is NOT misread as a sanitizer failure.
//
// SKIP semantics: if the toolchain cannot build an ASan executable, returns
// ran=false with a diagnostic beginning "runAotChecked: SKIP" so a caller can
// distinguish "environment can't run this" from "the program is dirty".
// ---------------------------------------------------------------------------
inline JitResult runAotCheckedImpl(std::string_view source,
                                    const char* name,
                                    bool detectLeaks,
                                    codegen::CodegenOptions opts = {}) {
    JitResult r;
    aot_detail::ensureInfraTallyAtExit();

    // SKIP if the C toolchain (which the driver shells out to for the ASan link)
    // is absent — preserves the pre-existing "environment can't run this"
    // contract. Memoized: probed once per process.
    //
    // #3007 scope note: this SKIP is itself a member of the #3633 silent-skip
    // class — a box without clang runs the whole ASan tier vacuously — but it
    // is DELIBERATELY left as-is here. It is already textually distinguishable
    // ("<name>: SKIP — C toolchain ..."), 224 call sites in 224 files read that
    // literal out of the message (`message.find("SKIP")`) and return without
    // asserting — measured 2026-09-03; do NOT confuse this set with the ~122
    // per-file native `runUnderAsan` helpers of #3626/#3633, which are a
    // different family, nor with the 1,551 `runAotChecked(` call sites that are
    // the total surface the SKIP disarms (issue 3733). Turning it loud is a
    // policy change about which environments may run the suite, not a
    // harness-honesty fix. Filed separately; this lane changes only the paths
    // that were INdistinguishable.
    static const bool toolchainOk = [] {
        return std::system("clang --version > /dev/null 2>&1") == 0;
    }();
    if (!toolchainOk) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             std::string(name) + ": SKIP — C toolchain (clang) "
                             "unavailable for the ASan link");
        return r;
    }

    std::string infraWhy;
    aot_detail::WorkDir wd{aot_detail::makeWorkDir(&infraWhy)};
    if (wd.path.empty())
        return aot_detail::markInfra(r, name, infraWhy);
    namespace fs = aot_detail::fs;
    const std::string srcPath = (fs::path(wd.path) / "prog.bzl").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string ccErr   = (fs::path(wd.path) / "cc.err").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string sanPath = (fs::path(wd.path) / "san.txt").string();

    if (!aot_detail::writeSource(srcPath, source, &infraWhy))
        return aot_detail::markInfra(r, name, infraWhy);

    // --- Compile + link with generated-code ASan, forked (S3), served from the
    //     link-cache (keyed sanitizeAddress=1, so it never collides with a plain
    //     build of the same source). ---------------------------------------
    // `opts` arrives from the caller (default-constructed for the plain
    // runAotChecked); this harness owns exactly one bit of it.
    opts.sanitizeAddress = true;
    aot_detail::CompileStatus cc =
        aot_detail::cachedCompile(source, srcPath, exePath, opts, ccErr);
    if (cc.outcome != aot_detail::CompileOutcome::Ok) {
        // fix #3007: an Infra outcome (refused fork, or a driver that reported
        // success and produced no executable) used to be dressed as
        // "<name>: SKIP — ..." at Severity::Error. That is the #3633 shape
        // twice over: an Error a negative assertion reads as a rejection, AND a
        // SKIP marker that 224 call sites read as "return without asserting".
        // It is infrastructure, so it goes to the infra channel and fails loudly.
        if (cc.outcome == aot_detail::CompileOutcome::Infra)
            return aot_detail::markInfra(r, name, cc.diag);
        // #2214: prefix a genuine verdict (Rejected / Crashed / TimedOut) with
        // the harness name so the diagnostic says which harness produced it.
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             std::string(name) + ": " + std::move(cc.diag));
        return r;  // reject / compiler crash / compile timeout — ran=false
    }

    // --- Run under (optionally) LeakSanitizer. abort_on_error=0 so a sanitizer
    //     error prints a REPORT and exits 99 (rather than SIGABRT); detection
    //     keys on the report text (M3), not the exit code. When detectLeaks is
    //     false (the *Lenient variant) LeakSanitizer is disabled at runtime
    //     (detect_leaks=0); the generated-code AddressSanitizer is still
    //     active, so UAF / OOB in emitted loads still trips. ---------------
    const std::string asanOpts = std::string("detect_leaks=") +
                                 (detectLeaks ? "1" : "0") +
                                 ":exitcode=99:abort_on_error=0";
    bool timedOut = false;
    int execErrno = 0;
    int status = aot_detail::spawnCaptured(
        {exePath}, outPath, sanPath, aot_detail::aotTimeoutSec(), timedOut,
        {{"ASAN_OPTIONS", asanOpts}}, &execErrno);
    const int spawnErrno = errno;   // only meaningful when status < 0
    // fix #3007: neither a failed fork() nor a failed execv() is a sanitizer
    // finding. Read as a wait status, spawnCaptured's -1 makes WIFSIGNALED true
    // and `crashed` true, which this helper reports as "sanitizer fired
    // (crash/abort)" — a fabricated memory-safety failure on a program that
    // never started.
    if (status < 0)
        return aot_detail::markInfra(
            r, name, "fork() failed launching the compiled program (errno=" +
                         std::to_string(spawnErrno) + ": " +
                         std::strerror(spawnErrno) + ")");
    if (execErrno != 0)
        return aot_detail::markInfra(
            r, name, "could not exec the linked program " + exePath +
                         " (errno=" + std::to_string(execErrno) + ": " +
                         std::strerror(execErrno) + ")");
    r.stdoutText = aot_detail::slurp(outPath);
    std::string sanText = aot_detail::slurp(sanPath);

    if (timedOut) {
        r.exitCode = -1;
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             std::string(name) + ": program timed out");
        return r;
    }

    const bool crashed = WIFSIGNALED(status);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    // qr1 M3: a REAL sanitizer failure is signalled by the report text (always
    // present with abort_on_error=0), OR by a hard crash that bypassed ASan's
    // own reporting. The bare exit code 99 is NOT used as a signal — a program
    // whose Main legitimately returns 99 stays a clean run.
    const bool sanitizerFired =
        crashed || aot_detail::hasSanitizerReport(sanText);

    if (sanitizerFired) {
        // #1822: in the whole-suite sweep, re-emit the report to the harness
        // stderr so scripts/asan_sweep.sh's grep catches a runAotChecked
        // detection too (otherwise it surfaces only as this test's own FAIL,
        // which the sweep's report-marker grep would miss).
        if (aot_detail::forceSanitizeAddress() && !sanText.empty())
            aot_detail::emitSanitizerReportToStderr(sanText);
        // A leak / double-free / UAF is a dispose-exactly-once violation: FAIL.
        r.exitCode = crashed ? -1 : code;
        std::string msg = std::string(name) + ": sanitizer fired (";
        msg += crashed ? "crash/abort"
                       : (detectLeaks ? "LSan/ASan at exit" : "ASan at exit");
        msg += ")";
        if (!sanText.empty()) {
            const std::size_t cap = 8192;
            msg += "\n--- sanitizer report ---\n" +
                   sanText.substr(0, std::min(sanText.size(), cap));
        }
        r.diagnostics.report(Severity::Error, SourceLocation{}, std::move(msg));
        return r;
    }

    // Clean run: dispose-exactly-once verified for this program (runAotChecked).
    // runAotCheckedLenient makes no dispose-exactly-once claim — the program
    // is only checked for crashes / generated-code UAF / OOB, not for leaks.
    r.exitCode = code;
    r.ran = WIFEXITED(status);
    return r;
}

inline JitResult runAotChecked(std::string_view source) {
    return runAotCheckedImpl(source, "runAotChecked", /*detectLeaks=*/true);
}

// runAotChecked with extra front-end/codegen options (the ASan/LSan bit is
// still this harness's own). Used by tests that must exercise a non-default
// compiler mode under the memory net - e.g. the proposal 0082 stage-1
// prototype flag (#3048), whose whole subject is disposal timing.
inline JitResult runAotChecked(std::string_view source,
                               const codegen::CodegenOptions& opts) {
    return runAotCheckedImpl(source, "runAotChecked", /*detectLeaks=*/true, opts);
}

// runAotCheckedLenient — issue #2214. Like runAotChecked but with
// LeakSanitizer DISABLED at runtime (detect_leaks=0); the generated-code
// AddressSanitizer (use-after-free / out-of-bounds in emitted loads) is still
// active. Use this for tests whose program intentionally or incidentally leaks
// (e.g. exercising for-in over an IOwningEnumerable<Drain>, where the for-in
// enumerator's own lifecycle is what's being tested and a leak in the test
// scaffold would otherwise trip the leak sentinel before the iteration count
// can be observed). The test program is NOT verified leak-clean under this
// helper — use runAotChecked for the ASan/LSan memory net. Crash and
// generated-code-ASan detection are unchanged: a real sanitizer error or a
// process crash still surfaces as ran=false with the report in `diagnostics`.
inline JitResult runAotCheckedLenient(std::string_view source) {
    return runAotCheckedImpl(source, "runAotCheckedLenient", /*detectLeaks=*/false);
}

}  // namespace bezel::test
