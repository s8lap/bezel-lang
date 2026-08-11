// Regression pin for issue 3014 (test-infra, C++ sink half): when
// BEZEL_AOT_CACHE_STATS_FILE is set, the [aot-cache] stats line is written
// ATOMICALLY to that file and NEVER to stderr. This is the structural fix for
// issue 3012, where the atexit hook's stderr print raced the shard's own summary
// printer mid-line and corrupted the "N passed, M failed" parse (a shard that
// ran 102 real tests contributed zero because its summary matched no pattern).
//
// This test forks a child that runs a real compile (populating the counters),
// then calls emitCacheStatsNow (the EXACT function the atexit hook calls) with
// the child's stderr redirected to a capture file. It asserts:
//   1. the stats FILE exists and carries hits=/misses=/store_failures= (plus the
//      new store_fsyncs= / store_errno= fields), AND
//   2. the child's captured stderr contains NO [aot-cache] line — the #3012
//      proof that the shard-summary stream is untouched.
//
// Fail-without / pass-with: with the file sink wired into emitCacheStatsNow this
// passes; reverting emitCacheStatsNow to always-stderr makes assertion 2 fail
// ([aot-cache] reappears on the shard's stream).
//
// A second test exercises the failure path directly: storeToCache under a
// nonexistent parent dir fails both hardlink and copy, so store_failures
// increments and the errno is captured — the telemetry that makes the #3003
// silent-store-failure class visible.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::codegen::CodegenOptions;
namespace aot = bezel::test::aot_detail;

namespace {

std::uint64_t nextNonce() {
    static std::uint64_t n =
        (static_cast<std::uint64_t>(::getpid()) << 20) ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    return n++;
}

// A valid program whose source bytes embed `tag` (distinct tags key distinctly)
// and whose Main returns `ret` (so a served binary is behaviourally checkable).
std::string prog(const std::string& tag, int ret) {
    return "module M; /* " + tag +
           " */ class C public static method Main(): int "
           "  return " +
           std::to_string(ret) + "; end Main; end C;";
}

bool cacheActive() {
    return aot::cacheEnabled() &&
           !aot::computeCacheKey("probe", CodegenOptions{}).empty();
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

}  // namespace

// The structural #3012 proof: with BEZEL_AOT_CACHE_STATS_FILE set, the
// [aot-cache] line goes to the FILE sink and stays OFF the shard's stderr.
BEZEL_TEST(aot_cache_stats_sink, file_sink_keeps_stderr_clean_spec_3012) {
    if (!cacheActive()) return;

    const std::string dir =
        aot::scratchBase() + "/stats_sink_" + std::to_string(nextNonce());
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string statsFile = dir + "/stats.txt";
    const std::string stderrCapture = dir + "/child_stderr.txt";

    // Route stats to the file sink. cacheStatsFile() reads this fresh, so the
    // forked child (which inherits the env) sees it too.
    ::setenv("BEZEL_AOT_CACHE_STATS_FILE", statsFile.c_str(), /*overwrite=*/1);

    // Flush the test framework's stdout (it printed "[ RUN ]" for this test)
    // before forking, or the child inherits the buffered bytes and re-emits them
    // when its own output path flushes — a cosmetic double "[ RUN ]" line.
    std::cout.flush();
    std::fflush(stdout);
    pid_t pid = ::fork();
    BEZEL_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        // child: send the shard-summary stream (stderr) to a capture file, run a
        // real compile to populate the counters, then emit stats via the EXACT
        // atexit-hook body. _exit (NOT exit): we already emitted, so avoid both
        // firing static destructors and a second emit from a real atexit hook.
        const int efd = ::open(stderrCapture.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0) {
            ::dup2(efd, STDERR_FILENO);
            ::close(efd);
        }
        JitResult r =
            runAot(prog("stats_sink_" + std::to_string(nextNonce()), 5));
        aot::emitCacheStatsNow();   // the atexit hook's body, called explicitly
        ::_exit(r.ran && r.exitCode == 5 ? 0 : 1);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    ::unsetenv("BEZEL_AOT_CACHE_STATS_FILE");

    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    BEZEL_EXPECT_EQ(WEXITSTATUS(status), 0);

    // 1) The stats FILE is populated with the expected fields (atomic write:
    //    temp + fsync + rename, so the reader sees exactly one whole line).
    const std::string stats = readFile(statsFile);
    BEZEL_EXPECT_TRUE(stats.find("[aot-cache]") != std::string::npos);
    BEZEL_EXPECT_TRUE(stats.find("hits=") != std::string::npos);
    BEZEL_EXPECT_TRUE(stats.find("misses=") != std::string::npos);
    BEZEL_EXPECT_TRUE(stats.find("store_failures=") != std::string::npos);
    BEZEL_EXPECT_TRUE(stats.find("store_fsyncs=") != std::string::npos);

    // 2) #3012 proof: the shard-summary stream (stderr) has NO [aot-cache] line.
    //    This is the line that used to race the shard summary mid-line.
    const std::string childStderr = readFile(stderrCapture);
    BEZEL_EXPECT_TRUE(childStderr.find("[aot-cache]") == std::string::npos);
}

// #3014: a failing store increments store_failures and captures the errno.
// storeToCache under a parent dir that does not exist fails both hardlink and
// copy (ENOENT), so the failure path runs. This is the telemetry that makes the
// #3003 silent-store-failure class (thousands of failed stores per shard, zero
// signal) visible in the stats sink.
BEZEL_TEST(aot_cache_stats_sink, failing_store_increments_store_failures) {
    const std::string exe =
        aot::scratchBase() + "/sf_exe_" + std::to_string(nextNonce());
    const std::string cachePath =
        aot::scratchBase() + "/no_such_dir_" + std::to_string(nextNonce()) +
        "/entry";
    {
        std::ofstream f(exe, std::ios::binary | std::ios::trunc);
        BEZEL_EXPECT_TRUE(f.good());
        f.close();
    }
    const std::uint64_t fails0 = aot::cacheStats().storeFailures.load();
    aot::storeToCache(exe, cachePath);
    BEZEL_EXPECT_TRUE(aot::cacheStats().storeFailures.load() > fails0);
    BEZEL_EXPECT_TRUE(aot::cacheStats().lastStoreErrno.load() != 0);
}
