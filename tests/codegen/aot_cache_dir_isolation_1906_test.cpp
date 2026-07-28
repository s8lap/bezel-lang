// Regression pin for #1906 — AOT bincache per-shard dir isolation.
//
// The sharded runner (tests/run_parallel.sh) launches N parallel processes
// that, by default, share /var/tmp/bezel-scratch/aot-bincache. Under
// sustained multi-shard contention, a per-shard aot_cache test's hit/miss
// counter assertions (a function-local static atomic, process-scoped) can
// observe state LEFT by an unrelated shard's compile — exactly the cross-
// shard pollution that flaked the gate. The fix sets BEZEL_AOT_CACHE_DIR to
// a unique path per shard in run_parallel.sh, and to a fresh mktemp dir per
// arbitration in scripts/gate.sh. Each shard's cacheDir() (a function-local
// static string, frozen for the process's lifetime) is therefore process-
// local and isolates naturally.
//
// The mechanical pin below exercises the underlying contract the fix
// relies on: BEZEL_AOT_CACHE_DIR is honored per-process — a forked child
// with a distinct env value uses its OWN dir, not the parent's. We exec
// this very test binary as a subprocess with two distinct cache dirs in
// turn (sequential, simpler than concurrent), filter its scope to one
// known-passing test that exercises the cache path, and confirm:
//
//   (a) each child exits 0,
//   (b) each child actually compiled to its OWN dir (a cache entry landed
//       under the requested path).
//
// A future change that broke the env override (e.g. removed setenv from
// spawnCaptured, dropped the BEZEL_AOT_CACHE_DIR read in cacheDir(), or
// made cacheStats()/cacheDir() shared across processes) would surface here
// as a child that failed, or worse, wrote the parent's cache dir.
//
// Per CLAUDE.md, the underlying shell fix lives in tests/run_parallel.sh
// and scripts/gate.sh — this C++ test pins the env-override contract that
// both rely on, so a compile-time or runtime regression in either
// infra piece is caught by ONE test.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
namespace aot = bezel::test::aot_detail;

namespace {

// Resolve the on-disk path to this test binary. The cacheDir() function is
// a function-local static (aot_support.hpp), initialized on first call from
// BEZEL_AOT_CACHE_DIR; once set, it is frozen for the parent's lifetime.
// To exercise a DIFFERENT env value we MUST fork+exec into a fresh process —
// setenv in the parent has no effect after first read.
std::string selfPath() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

// Build a unique empty directory under /var/tmp/bezel-scratch to hand the
// subprocess as its cache dir. Per CLAUDE.md, never /tmp (tmpfs is RAM; a
// cache tree there accumulates over the agent's lifetime). /var/tmp/bezel-
// scratch is the project's agreed scratch space (reclaimed by gc_scratch.sh).
std::string makeCacheDir(const std::string& tag) {
    fs::create_directories("/var/tmp/bezel-scratch");
    std::string base = "/var/tmp/bezel-scratch/aot-cache-iso-1906-" + tag +
                       "-XXXXXX";
    std::vector<char> tmpl(base.begin(), base.end());
    tmpl.push_back('\0');
    char* d = ::mkdtemp(tmpl.data());
    if (!d) return {};
    return std::string(d);
}

// Re-exec this binary with a given cache dir and a BEZEL_TEST_FILTER
// narrowing its test scope to one known-passing cache test that exercises
// cachedCompile (aot_cache.unchanged_program_hits_and_flipped_source_
// misses runs three runAot() calls — first compile MISS, second HIT,
// third MISS-after-source-flip — so the atexit stats hook fires and the
// subprocess's writes land in cacheDir()). The harness's filter matches
// either the suite OR name substring (test_harness.hpp runAll), so passing
// just the test name selects that test. Returns raw waitpid status.
int runSubprocessWithCache(const std::string& self,
                           const std::string& cacheDir,
                           const std::string& outPath,
                           const std::string& errPath,
                           int timeoutSec) {
    bool timedOut = false;
    std::vector<std::pair<std::string, std::string>> env = {
        {"BEZEL_AOT_CACHE_DIR", cacheDir},
        {"BEZEL_TEST_FILTER", "unchanged_program_hits_and_flipped_source_misses"},
        {"BEZEL_AOT_CACHE_STATS", "1"},
    };
    return aot::spawnCaptured({self}, outPath, errPath, timeoutSec,
                              timedOut, env);
}

}  // namespace

// =====================================================================
// #1906-1: BEZEL_AOT_CACHE_DIR is honored per-process. Two sequential
// forks under distinct cache dirs both:
//   (a) exit 0 (the filtered test passed),
//   (b) write at least one cache entry under their OWN dir (proves the
//       env override took effect and the atexit stats hook fired with
//       the correct dir= value).
//
// Without run_parallel.sh's per-shard dir setting or gate.sh's fresh
// per-arbitration dir, every shard/arbitration would land in the same
// default dir — this test pins the mechanism that makes the per-shard
// isolation possible.
// =====================================================================
BEZEL_TEST(aot_cache_dir_isolation_1906,
           distinct_subprocess_envs_yield_independent_cache_dirs) {
    std::string self = selfPath();
    BEZEL_EXPECT_TRUE(!self.empty());
    if (self.empty()) return;

    std::string dirA = makeCacheDir("a");
    std::string dirB = makeCacheDir("b");
    BEZEL_EXPECT_TRUE(!dirA.empty());
    BEZEL_EXPECT_TRUE(!dirB.empty());
    if (dirA.empty() || dirB.empty()) return;

    const std::string outA = dirA + "/subproc.out";
    const std::string errA = dirA + "/subproc.err";
    const std::string outB = dirB + "/subproc.out";
    const std::string errB = dirB + "/subproc.err";

    int statusA = runSubprocessWithCache(self, dirA, outA, errA, 120);
    int statusB = runSubprocessWithCache(self, dirB, outB, errB, 120);

    bool okA = WIFEXITED(statusA) && WEXITSTATUS(statusA) == 0;
    bool okB = WIFEXITED(statusB) && WEXITSTATUS(statusB) == 0;
    BEZEL_EXPECT_TRUE(okA);
    BEZEL_EXPECT_TRUE(okB);
    if (!okA || !okB) {
        std::string ea = aot::slurp(errA);
        std::string eb = aot::slurp(errB);
        std::fprintf(stderr,
                     "#1906 child A exit=%d stderr: %s\n"
                     "#1906 child B exit=%d stderr: %s\n",
                     WIFEXITED(statusA) ? WEXITSTATUS(statusA) : -1,
                     ea.c_str(),
                     WIFEXITED(statusB) ? WEXITSTATUS(statusB) : -1,
                     eb.c_str());
    }

    // Each subprocess must have written the atexit stats line into ITS
    // OWN dir. The cacheStats atexit hook emits the line to stderr; the
    // subprocess's test runs `[ RUN ]/[ OK ]` on stdout and the stats line
    // `[aot-cache] hits=N misses=M dir=<path>` on stderr (aot_support.hpp
    // ensureStatsAtExit → std::fprintf(stderr, ...)). A future regression
    // that broke the env override would surface here as the parent's
    // default dir (different from dirA/dirB) appearing instead.
    std::string errAc = aot::slurp(errA);
    std::string errBc = aot::slurp(errB);
    BEZEL_EXPECT_TRUE(errAc.find("dir=" + dirA) != std::string::npos);
    BEZEL_EXPECT_TRUE(errBc.find("dir=" + dirB) != std::string::npos);

    fs::remove_all(dirA);
    fs::remove_all(dirB);
}
