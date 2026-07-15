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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

#include "support/codegen_support.hpp"   // JitResult + codegen::CodegenOptions
#include "driver/Driver.hpp"             // Driver::compileFile (the AOT path)

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"         // content-addressed link-cache key

namespace bezel::test {

namespace aot_detail {

namespace fs = std::filesystem;

// Base scratch dir for per-run temp artifacts. Per CLAUDE.md: never /tmp (tmpfs
// is RAM; a scratch object tree there thrashes the box) — /var/tmp is disk.
inline std::string scratchBase() { return "/var/tmp/bezel-scratch/aot-harness"; }

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
inline int spawnCaptured(const std::vector<std::string>& argv,
                         const std::string& outPath, const std::string& errPath,
                         int timeoutSec, bool& timedOut,
                         const std::vector<std::pair<std::string, std::string>>&
                             extraEnv = {}) {
    timedOut = false;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
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
        ::execv(cargv[0], cargv.data());
        ::_Exit(127);  // exec failed
    }
    // ---- PARENT ----
    return reapWithDeadline(pid, timeoutSec, timedOut);
}

inline bool keepArtifacts() { return std::getenv("BEZEL_AOT_KEEP") != nullptr; }

// Minimum age (seconds) before a leftover work dir is eligible for the boot-time
// reaper (qr1 N1). Default 1800s (30 min): comfortably above the lifetime of any
// single runAot call (compile deadline 120s + run deadline 30s ≈ 150s worst
// case — a >10x margin) so the reaper can NEVER race a dir that a concurrent
// shard is still using. run_parallel.sh launches ~16 shards, each its own
// process, all starting together; each shard's reaper runs ONCE at its first
// makeWorkDir() (≈ process start), when the only aot* dirs older than this
// threshold are genuine leaks from a PRIOR run. BEZEL_AOT_REAP_AGE_SEC overrides
// (lower only in a SERIAL run — a small value is unsafe under concurrent
// shards); 0 disables the reaper.
inline int reapAgeSec() {
    static const int v = [] {
        const char* e = std::getenv("BEZEL_AOT_REAP_AGE_SEC");
        if (!e || *e == '\0') return 1800;
        int n = std::atoi(e);
        return n >= 0 ? n : 1800;
    }();
    return v;
}

// qr1 N1: reap STALE leftover work dirs once per process. WorkDir RAII cleans a
// work dir on every NORMAL path — including the compile/run child-timeout paths,
// where the PARENT survives the child's SIGKILL and its dtor still runs. The
// leak this closes is the WHOLE PROCESS being killed: the outer
// BEZEL_SHARD_TIMEOUT (300s) SIGKILLs a wedged shard, or the dev_heavy memory
// cage OOM-kills it — the dtor never runs and the mkdtemp dir leaks under
// scratchBase(). Over a 7,500-test migration these accumulate in /var/tmp. On
// the first makeWorkDir() of each process we sweep aot* dirs older than
// reapAgeSec(); the AGE GATE (see reapAgeSec) means only prior-run leaks are
// reaped, never a sibling shard's in-flight dir.
inline void reapStaleWorkDirs() {
    static const bool once = [] {
        const int ageSec = reapAgeSec();
        if (ageSec <= 0 || keepArtifacts()) return true;  // disabled / keep mode
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        const auto maxAge = std::chrono::seconds(ageSec);
        for (fs::directory_iterator it(scratchBase(), ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ecEntry;
            const fs::path& p = it->path();
            // Only our own mkdtemp dirs ("aotXXXXXX"): never touch a sibling
            // tool's scratch that may share the base directory.
            if (p.filename().string().rfind("aot", 0) != 0) continue;
            if (!fs::is_directory(p, ecEntry) || ecEntry) continue;
            const auto mt = fs::last_write_time(p, ecEntry);
            if (ecEntry) continue;
            if (now - mt > maxAge) fs::remove_all(p, ecEntry);  // best-effort
        }
        return true;
    }();
    (void)once;
}

// A per-call working directory under scratchBase(). Empty string on failure.
inline std::string makeWorkDir() {
    std::error_code ec;
    fs::create_directories(scratchBase(), ec);
    reapStaleWorkDirs();  // qr1 N1: sweep prior-run leaks once per process.
    std::string tmpl = scratchBase() + "/aotXXXXXX";
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back('\0');
    const char* dirp = ::mkdtemp(tbuf.data());
    return dirp ? std::string(dirp) : std::string{};
}

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
inline bool forceSanitizeAddress() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORCE_SANITIZE");
        if (!e || *e == '\0') return false;
        std::string s(e);
        return s == "address" || s == "asan" || s == "1";
    }();
    return v;
}

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

// Compile `srcPath` to the native executable `exePath` via `Driver::compileFile`
// run in a fork()ed CHILD (crash- and hang-isolated, qr1 M1). `opts` selects the
// codegen mode (e.g. sanitizeAddress for the checked leg — S3). The child's
// stderr (diagnostics + any clang link error) is captured, bounded, into the
// result. fork() failure is FATAL by default (#1861: refuse the un-isolated
// compile so process isolation is never silently lost — the basis for the
// shard-authoritative gate having no serial backstop); it is always LOUD
// (noteForkFailureDegradation), and BEZEL_AOT_FORKFAIL_FATAL=0 opts back into a
// graceful in-process (un-isolated) fallback — same policy runJit had.
inline CompileStatus forkedCompile(const std::string& srcPath,
                                   const std::string& exePath,
                                   const codegen::CodegenOptions& opts,
                                   const std::string& ccErrPath) {
    // Bound machine-wide concurrent compiles (#1780). Held for the whole
    // compile+link (both the forked and the fork-failure in-process path) and
    // released when this function returns — including on timeout/crash, since
    // the parent always reaps before returning. Acquired BEFORE the fork so the
    // front-end + clang/ld link all run under the one token.
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
        Driver driver;
        CompileResult cc = driver.compileFile(srcPath, exePath, /*deps=*/{}, opts);
        if (saved >= 0) {
            std::fflush(stderr);
            std::cerr.flush();
            ::dup2(saved, STDERR_FILENO);
            ::close(saved);
        }
        std::string err = slurp(ccErrPath);
        if (cc.success && fs::exists(exePath)) return {CompileOutcome::Ok, err, 0};
        return rejectFrom(std::move(err), cc.success ? 0 : 1);
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

        Driver driver;
        CompileResult cc = driver.compileFile(srcPath, exePath, /*deps=*/{}, opts);
        std::fflush(stderr);
        std::cerr.flush();
        // _Exit (NOT exit): skip atexit handlers and static destructors in the
        // forked copy — the child's whole heap + LLVM allocator state is
        // reclaimed by the OS, keeping the parent's RSS flat (qr1 E1). Same
        // discipline as runJit's fork.
        std::_Exit(cc.success ? 0 : 1);
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
        // exit with no executable (would otherwise run a missing/stale exe).
        if (!fs::exists(exePath)) {
            return {CompileOutcome::Infra,
                    "runAot: driver reported success but produced no executable "
                    "at " + exePath, 0};
        }
        return {CompileOutcome::Ok, err, 0};
    }
    return rejectFrom(std::move(err), code);
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
//   (a) COMPILER IDENTITY — the test binary itself (/proc/self/exe). The
//       compile runs IN-PROCESS (Driver::compileFile) inside bezel_tests, so
//       the running binary's own bytes are the soundest identity: any change to
//       the front-end, codegen, or the whole-archived bezel_runtime relinks
//       bezel_tests and changes this hash. Conservative — a test-only edit also
//       relinks bezel_tests and invalidates the whole cache — but it can NEVER
//       yield a stale hit. Soundness first (a refinement that hashes only the
//       compiler libs is possible follow-up, not done here).
//   (b) RUNTIME-LIBRARY IDENTITY — libbezel_runtime.a (BEZEL_RUNTIME_LIB_PATH),
//       what the driver's link actually consumes. Redundant with (a) in a
//       normal build (the archive is whole-archived INTO bezel_tests), but it
//       closes the window where the .a is rebuilt WITHOUT relinking the running
//       bezel_tests: then (a) is unchanged yet a fresh link would pull a
//       DIFFERENT runtime. Hashing the .a directly keeps that from ever being a
//       stale hit. (If the macro is somehow undefined, (a) still covers the
//       whole-archived runtime, so the key stays sound — just one layer thinner.)
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
// STORE: <cacheDir>/<first2>/<hash>, installed atomically (hardlink|copy to a
// temp in the same dir, then rename) so concurrent shards race benignly. A hit
// hardlinks (or, across a filesystem boundary, copies) the cached exe into the
// test's workdir and runs it as a fresh build would. A hit performs NO link, so
// it does NOT consume a #1780 LinkToken.
//
// KNOBS: BEZEL_AOT_CACHE=0 disables (default ON); BEZEL_AOT_CACHE_DIR overrides
// the location; BEZEL_AOT_CACHE_STATS=1 prints a hits/misses line at process
// (shard) exit. GC: once per process, under an flock so only one shard sweeps,
// if the store exceeds ~5 GiB the oldest-mtime entries are deleted down to half
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

inline const std::string& cacheDir() {
    // Leaked on purpose (never destroyed): the BEZEL_AOT_CACHE_STATS atexit hook
    // reads this AT PROCESS TEARDOWN, after ordinary function-local statics have
    // already run their destructors. A leaked object has no destructor to race.
    static const std::string* v = [] {
        const char* e = std::getenv("BEZEL_AOT_CACHE_DIR");
        return new std::string((e && *e) ? e
                                         : "/var/tmp/bezel-scratch/aot-bincache");
    }();
    return *v;
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

// SHA-256(compiler-identity ‖ runtime-library-identity), computed ONCE per
// process (hashing /proc/self/exe is a few hundred ms; amortized over the whole
// shard it is negligible). Returns nullptr if the test binary cannot be hashed
// — with no sound compiler identity the cache DISABLES itself rather than risk a
// stale hit.
inline const std::array<uint8_t, 32>* combinedIdentity() {
    static std::array<uint8_t, 32> out{};
    static const bool ok = [] {
        std::array<uint8_t, 32> exeHash;
        if (!sha256File("/proc/self/exe", exeHash)) return false;
        llvm::SHA256 h;
        h.update(llvm::ArrayRef<uint8_t>(exeHash.data(), exeHash.size()));
#ifdef BEZEL_RUNTIME_LIB_PATH
        // (b): fold in the runtime archive the driver's link consumes.
        std::array<uint8_t, 32> rtHash;
        if (sha256File(BEZEL_RUNTIME_LIB_PATH, rtHash))
            h.update(llvm::ArrayRef<uint8_t>(rtHash.data(), rtHash.size()));
#endif
        out = h.final();
        return true;
    }();
    return ok ? &out : nullptr;
}

// The mode component of the key (c): every CodegenOptions bit that alters the
// emitted object or the link line. The "k1;" schema tag is bumped if the key
// composition ever changes, so old entries fall out cleanly.
inline std::string modeString(const codegen::CodegenOptions& opts) {
    std::string s = "k1;";
    s += "sa=";  s += opts.sanitizeAddress ? '1' : '0';
    s += ";cd="; s += opts.checkedDisposal ? '1' : '0';
    s += ";mm="; s += opts.isMainModule ? '1' : '0';
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

// Per-process hit/miss counters (a hit is served from the cache; a miss ran the
// real compiler — including reject/crash/timeout, which are never cacheable).
struct CacheStats {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
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

// Register (once) an atexit hook that prints the hits/misses line at shard exit
// when BEZEL_AOT_CACHE_STATS=1. Default off → normal runs print nothing.
inline void ensureStatsAtExit() {
    static const bool once = [] {
        if (cacheStatsEnabled()) {
            std::atexit([] {
                auto& s = cacheStats();
                unsigned long long h = s.hits.load(std::memory_order_relaxed);
                unsigned long long m = s.misses.load(std::memory_order_relaxed);
                std::fprintf(stderr,
                    "[aot-cache] hits=%llu misses=%llu (%.0f%% hit) dir=%s\n",
                    h, m, (h + m) ? 100.0 * h / (h + m) : 0.0,
                    cacheDir().c_str());
            });
        }
        return true;
    }();
    (void)once;
}

// Install a cached executable into the test's workdir at `exePath`: a hardlink
// (no byte copy; the cache and workdir share /var/tmp) or, across a filesystem
// boundary (a custom BEZEL_AOT_CACHE_DIR), a copy. Returns false on failure, in
// which case the caller falls back to a real compile.
inline bool installFromCache(const std::string& cachePath,
                             const std::string& exePath) {
    std::error_code ec;
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
        if (ec) return;
    }
    fs::permissions(tmp, kExePerms, fs::perm_options::replace, ec);
    ec.clear();
    fs::rename(tmp, cachePath, ec);  // atomic install (same filesystem)
    if (ec) { std::error_code e2; fs::remove(tmp, e2); }  // clean the temp
}

// Size-bounded GC, run ONCE per process under a non-blocking flock so only one
// shard sweeps (losers skip; GC is a coarse safety valve, not per-run-critical).
// If the store exceeds ~5 GiB, delete oldest-mtime entries down to half.
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

        const std::uintmax_t cap = static_cast<std::uintmax_t>(5) << 30;  // 5 GiB
        const std::uintmax_t target = cap / 2;                            // 2.5 GiB
        struct Ent { std::string path; std::uintmax_t size; fs::file_time_type mt; };
        std::vector<Ent> ents;
        std::uintmax_t total = 0;
        for (fs::recursive_directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) continue;
            const std::string name = it->path().filename().string();
            if (name == ".gc.lock" || name.rfind(".tmp.", 0) == 0) continue;
            std::uintmax_t sz = fs::file_size(it->path(), e2); if (e2) continue;
            fs::file_time_type mt = fs::last_write_time(it->path(), e2);
            if (e2) continue;
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

// Compile `srcPath` to `exePath`, serving a cached link product when the key
// (compiler ‖ runtime ‖ mode ‖ source) already has one. A hit runs NO link (and
// no #1780 token); a miss delegates to forkedCompile and, on success, installs
// the product. On a compile REJECT/CRASH/TIMEOUT nothing is cached (the real
// diagnostics stay live). The returned CompileStatus is indistinguishable from
// forkedCompile's — a hit yields {Ok, "", 0} with exePath populated.
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
    if (std::getenv("BEZEL_AOT_TEST_CRASH_HOOK"))
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    if (!cacheEnabled())
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    ensureStatsAtExit();
    const std::string key = computeCacheKey(source, opts);
    if (key.empty())  // no sound compiler identity → never risk a stale hit
        return forkedCompile(srcPath, exePath, opts, ccErrPath);

    maybeGcCache();  // once per process (guarded)

    const std::string sub = cacheDir() + "/" + key.substr(0, 2);
    const std::string cachePath = sub + "/" + key;

    std::error_code ec;
    if (fs::exists(cachePath, ec) && !ec) {
        if (installFromCache(cachePath, exePath)) {
            cacheStats().hits.fetch_add(1, std::memory_order_relaxed);
            // Best-effort LRU touch so a hot entry survives the size GC.
            std::error_code e2;
            fs::last_write_time(cachePath,
                                fs::file_time_type::clock::now(), e2);
            return {CompileOutcome::Ok, {}, 0};
        }
        // Install failed (disk error / vanished entry) → fall through to a real
        // compile rather than fail the test.
    }

    CompileStatus cc = forkedCompile(srcPath, exePath, opts, ccErrPath);
    cacheStats().misses.fetch_add(1, std::memory_order_relaxed);
    if (cc.outcome == CompileOutcome::Ok) {
        fs::create_directories(sub, ec);
        storeToCache(exePath, cachePath);  // best-effort atomic install
    }
    return cc;
}

// Write `source` to `srcPath`. Returns false on write failure (disk full etc.).
inline bool writeSource(const std::string& srcPath, std::string_view source) {
    std::ofstream os(srcPath, std::ios::binary);
    os.write(source.data(), static_cast<std::streamsize>(source.size()));
    os.flush();
    return static_cast<bool>(os);  // qr1 N4: report a failed write as infra
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

    // #1822: in the whole-suite ASan sweep, force generated-code ASan on every
    // program (the runAotChecked recipe, suite-wide). A local copy keeps the
    // public signature and every call site unchanged.
    codegen::CodegenOptions opts = optsIn;
    if (aot_detail::forceSanitizeAddress()) opts.sanitizeAddress = true;

    aot_detail::WorkDir wd{aot_detail::makeWorkDir()};
    if (wd.path.empty()) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAot: could not create work dir under " +
                                 aot_detail::scratchBase());
        return r;
    }
    namespace fs = aot_detail::fs;
    const std::string srcPath = (fs::path(wd.path) / "prog.bzl").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string ccErr   = (fs::path(wd.path) / "cc.err").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string errPath = (fs::path(wd.path) / "run.err").string();

    if (!aot_detail::writeSource(srcPath, source)) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAot: failed to write source to " + srcPath);
        return r;
    }

    // --- Compile + link via the real driver, forked (crash/hang isolated),
    //     served from the content-addressed link-cache on an unchanged input. --
    aot_detail::CompileStatus cc =
        aot_detail::cachedCompile(source, srcPath, exePath, opts, ccErr);
    if (cc.outcome != aot_detail::CompileOutcome::Ok) {
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
    int status = aot_detail::spawnCaptured({exePath}, outPath, errPath,
                                           aot_detail::aotTimeoutSec(), timedOut,
                                           runEnv);
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
inline JitResult runAotChecked(std::string_view source) {
    JitResult r;

    // SKIP if the C toolchain (which the driver shells out to for the ASan link)
    // is absent — preserves the pre-existing "environment can't run this"
    // contract. Memoized: probed once per process.
    static const bool toolchainOk = [] {
        return std::system("clang --version > /dev/null 2>&1") == 0;
    }();
    if (!toolchainOk) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: SKIP — C toolchain (clang) "
                             "unavailable for the ASan link");
        return r;
    }

    aot_detail::WorkDir wd{aot_detail::makeWorkDir()};
    if (wd.path.empty()) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: could not create work dir");
        return r;
    }
    namespace fs = aot_detail::fs;
    const std::string srcPath = (fs::path(wd.path) / "prog.bzl").string();
    const std::string exePath = (fs::path(wd.path) / "prog").string();
    const std::string ccErr   = (fs::path(wd.path) / "cc.err").string();
    const std::string outPath = (fs::path(wd.path) / "out.txt").string();
    const std::string sanPath = (fs::path(wd.path) / "san.txt").string();

    if (!aot_detail::writeSource(srcPath, source)) {
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: failed to write source");
        return r;
    }

    // --- Compile + link with generated-code ASan, forked (S3), served from the
    //     link-cache (keyed sanitizeAddress=1, so it never collides with a plain
    //     build of the same source). ---------------------------------------
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    aot_detail::CompileStatus cc =
        aot_detail::cachedCompile(source, srcPath, exePath, opts, ccErr);
    if (cc.outcome != aot_detail::CompileOutcome::Ok) {
        std::string prefix =
            cc.outcome == aot_detail::CompileOutcome::Infra
                ? "runAotChecked: SKIP — "
                : "";
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             prefix + std::move(cc.diag));
        return r;  // reject / compiler crash / compile timeout — ran=false
    }

    // --- Run under LeakSanitizer. abort_on_error=0 so a sanitizer error prints
    //     a REPORT and exits 99 (rather than SIGABRT); detection keys on the
    //     report text (M3), not the exit code. --------------------------------
    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {exePath}, outPath, sanPath, aot_detail::aotTimeoutSec(), timedOut,
        {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    r.stdoutText = aot_detail::slurp(outPath);
    std::string sanText = aot_detail::slurp(sanPath);

    if (timedOut) {
        r.exitCode = -1;
        r.diagnostics.report(Severity::Error, SourceLocation{},
                             "runAotChecked: program timed out");
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
        std::string msg = "runAotChecked: sanitizer fired (";
        msg += crashed ? "crash/abort" : "LSan/ASan at exit";
        msg += ")";
        if (!sanText.empty()) {
            const std::size_t cap = 8192;
            msg += "\n--- sanitizer report ---\n" +
                   sanText.substr(0, std::min(sanText.size(), cap));
        }
        r.diagnostics.report(Severity::Error, SourceLocation{}, std::move(msg));
        return r;
    }

    // Clean run: dispose-exactly-once verified for this program.
    r.exitCode = code;
    r.ran = WIFEXITED(status);
    return r;
}

}  // namespace bezel::test
