// Pins the harness's content-keyed RESULTS cache (tests/support/results_cache.hpp,
// wired into tests/test_main.cpp) END TO END: each parent test re-execs THIS
// binary, filtered to a child "canary" row, against a private cache directory
// and a key file naming this binary, and reads what the child printed.
//
// The canary FAILS whenever its arm file exists and PASSES otherwise. The arm
// file's path is fixed for a parent test, so arming it changes nothing in the
// cache key: a run that reports the canary PASSED while it is armed can only
// have been served from the cache, and a run that reports it FAILED must have
// executed it. That turns "was this test served or executed?" into an
// observable verdict instead of a timing guess.
//
// What is pinned (each is a property a stale PASS would need to break):
//   * a recorded PASS is served without running the body;
//   * BEZEL_RESULTS_CACHE=0 — what every gate script and tests/run_parallel.sh
//     export — executes the body even with a valid key file present, and a run
//     with NO key file (the gate, a hand run) prints no cache line at all;
//   * a FAIL is never stored, so it re-runs every time;
//   * a recorded PASS contradicted by a later FAIL under the same key is
//     POISONED and never served again, even after the test passes again;
//   * the key is bound to this binary's identity (a key file for a different
//     stat id is ignored), to the behaviour-changing environment (a BEZEL_*
//     knob flipped is a miss), and the cache is off under
//     BEZEL_AOT_FORCE_SANITIZE;
//   * a PASS that moved a loudness counter (here: a native-sanitizer skip) is
//     not stored;
//   * the identity probe (BEZEL_RESULTS_CACHE_DESCRIBE) prints the binary's
//     stat id and runs nothing.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "native_test_harness.hpp"
#include "support/aot_support.hpp"
#include "support/results_cache.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace {

namespace aot = bezel::test::aot_detail;
namespace rc = bezel::test::results_cache;
namespace fs = std::filesystem;
using bezel::test::currentTestFailed;

// Recursion guard: set only in the children the parent tests spawn; the child
// rows below do nothing without it, and the parent rows do nothing with it.
constexpr const char* kChildGuard = "BEZEL_RESULTS_CACHE_SELFTEST_CHILD";
// The arm file's path (constant per parent test, so it never moves the key).
constexpr const char* kArmEnv = "BEZEL_RCACHE_SELFTEST_ARM";

std::string selfExe() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

struct Leg {
    int status = -1;
    std::string out;
    bool exitedZero() const { return WIFEXITED(status) && WEXITSTATUS(status) == 0; }
};

struct Fixture {
    fs::path dir;
    std::string exe, cacheDir, keyFile, armFile;

    explicit Fixture(const char* tag) {
        dir = wtScratchDir(tag);
        exe = selfExe();
        cacheDir = (dir / "cache").string();
        keyFile = (dir / "keys.tsv").string();
        armFile = (dir / "ARM").string();
        writeKeyFile(rc::statId("/proc/self/exe"));
    }
    void writeKeyFile(const std::string& stat) const {
        std::ofstream k(keyFile, std::ios::trunc);
        k << rc::kSchema << "\t" << stat << "\t"
          << rc::sha256Hex(std::string("results-cache-selftest:") + dir.string())
          << "\t" << exe << "\n";
    }
    void arm(bool on) const {
        if (on) std::ofstream(armFile) << "armed\n";
        else fs::remove(armFile);
    }
    // Run the child row `row` with the cache env (`mode` nullptr = no
    // BEZEL_RESULTS_CACHE at all; `keys` false = no key file at all).
    Leg run(const char* row, const char* mode, bool keys = true,
            std::vector<std::pair<std::string, std::string>> extra = {}) const {
        static int n = 0;
        const std::string outPath = (dir / ("leg" + std::to_string(++n) + ".out")).string();
        std::vector<std::pair<std::string, std::string>> env = {
            {kChildGuard, "1"},
            {kArmEnv, armFile},
            {"BEZEL_TEST_FILTER", row},
            {"BEZEL_TEST_LIST", ""},
            {"BEZEL_TEST_ENUMERATE", ""},
            {"BEZEL_SHARD_COUNT", ""},
            {"BEZEL_SHARD_INDEX", ""},
            {"BEZEL_SHARD_SUMMARY", ""},
            // The parent may itself run under the gate's ASan oracle; the
            // children must not, except in the leg that pins that very case.
            {"BEZEL_AOT_FORCE_SANITIZE", ""},
            {"BEZEL_RESULTS_CACHE_DIR", cacheDir},
            {"BEZEL_RESULTS_CACHE_KEYS", keys ? keyFile : ""},
            {"BEZEL_RESULTS_CACHE", mode ? mode : ""},
        };
        for (auto& kv : extra) env.push_back(kv);
        bool timedOut = false;
        int execErrno = 0;
        Leg l;
        l.status = aot::spawnCaptured({exe}, outPath, outPath, /*timeoutSec=*/120,
                                      timedOut, env, &execErrno);
        l.out = readAll(outPath);
        BEZEL_EXPECT_FALSE(timedOut);
        BEZEL_EXPECT_EQ(execErrno, 0);
        return l;
    }
    std::size_t passEntries() const {
        std::size_t n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(cacheDir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".pass") ++n;
        return n;
    }
};

// Full names (for the output checks) and the filters that select exactly them
// (BEZEL_TEST_FILTER matches a substring of the suite OR of the name).
constexpr const char* kCanary = "support_results_cache.rcache_child_canary";
constexpr const char* kCanaryRow = "rcache_child_canary";
constexpr const char* kLoudRow = "rcache_child_loud_pass";

void dumpOnFailure(const Leg& l, const char* what) {
    if (currentTestFailed())
        std::cout << "--- " << what << " ---\n" << l.out << "\n--- end ---\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Child rows (inert without the guard). The canary fails iff armed.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, rcache_child_canary) {
    if (const char* c = std::getenv(kChildGuard); !(c && *c)) return;
    const char* arm = std::getenv(kArmEnv);
    std::cout << "canary body executed\n";
    BEZEL_EXPECT_FALSE(arm && *arm && fs::exists(arm));
}

// A PASS that moved a loudness counter (a native-sanitizer skip): never stored.
BEZEL_TEST(support_results_cache, rcache_child_loud_pass) {
    if (const char* c = std::getenv(kChildGuard); !(c && *c)) return;
    ::bezel::native_test::nativeSanitizerUnavailableSkips().fetch_add(1);
}

// ---------------------------------------------------------------------------
// A recorded PASS is served without running the body; BEZEL_RESULTS_CACHE=0
// (the gate's setting) and a run with no key file both execute it.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, served_pass_skips_body_and_off_switch_executes) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-served");

    Leg cold = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_TRUE(cold.exitedZero());
    BEZEL_EXPECT_TRUE(has(cold.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(cold.out, "RESULTS CACHE: 0 reused, 1 executed, 1 stored, 0 poisoned"));
    BEZEL_EXPECT_EQ(f.passEntries(), std::size_t{1});
    dumpOnFailure(cold, "cold");

    f.arm(true);
    Leg warm = f.run(kCanaryRow, "1");
    // Armed, the body would FAIL: a PASS here can only be the served record.
    BEZEL_EXPECT_TRUE(warm.exitedZero());
    BEZEL_EXPECT_FALSE(has(warm.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(warm.out, std::string("[ CACHED ] ") + kCanary));
    BEZEL_EXPECT_TRUE(has(warm.out, std::string("[  OK  ] ") + kCanary));
    BEZEL_EXPECT_TRUE(has(warm.out, "RESULTS CACHE: 1 reused, 0 executed, 0 stored, 0 poisoned"));
    BEZEL_EXPECT_TRUE(has(warm.out, "1 passed, 0 failed"));
    dumpOnFailure(warm, "warm");

    // The gate's switch: a valid key file is present and is IGNORED.
    Leg off = f.run(kCanaryRow, "0");
    BEZEL_EXPECT_FALSE(off.exitedZero());
    BEZEL_EXPECT_TRUE(has(off.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(off.out, std::string("[ FAIL ] ") + kCanary));
    BEZEL_EXPECT_FALSE(has(off.out, "[ CACHED ]"));
    BEZEL_EXPECT_TRUE(has(off.out, "(off: BEZEL_RESULTS_CACHE=0)"));
    dumpOnFailure(off, "off");

    // No key file at all (a hand run, run_parallel.sh, the gate): executes,
    // and prints no cache line — the output is byte-identical to before.
    Leg bare = f.run(kCanaryRow, nullptr, /*keys=*/false);
    BEZEL_EXPECT_FALSE(bare.exitedZero());
    BEZEL_EXPECT_TRUE(has(bare.out, "canary body executed"));
    BEZEL_EXPECT_FALSE(has(bare.out, "RESULTS CACHE"));
    BEZEL_EXPECT_FALSE(has(bare.out, "[ CACHED ]"));
    dumpOnFailure(bare, "bare");
}

// ---------------------------------------------------------------------------
// A FAIL is never stored: armed, the canary fails on every run, and the cache
// directory never holds a .pass entry for it.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, failing_test_is_never_stored) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-fail");
    f.arm(true);
    for (int i = 0; i < 3; ++i) {
        Leg l = f.run(kCanaryRow, "1");
        BEZEL_EXPECT_FALSE(l.exitedZero());
        BEZEL_EXPECT_TRUE(has(l.out, "canary body executed"));
        BEZEL_EXPECT_TRUE(has(l.out, "RESULTS CACHE: 0 reused, 1 executed, 0 stored, 0 poisoned"));
        dumpOnFailure(l, "armed");
    }
    BEZEL_EXPECT_EQ(f.passEntries(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Flake guard: a recorded PASS contradicted by a FAIL under the same key is
// poisoned, logged, and never served again, even once the test passes again.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, contradicted_pass_is_poisoned_for_good) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-poison");

    Leg seed = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_TRUE(seed.exitedZero());
    BEZEL_EXPECT_TRUE(has(seed.out, "1 stored"));

    f.arm(true);
    // verify = never serve, always execute, still record: the contradiction.
    Leg contra = f.run(kCanaryRow, "verify");
    BEZEL_EXPECT_FALSE(contra.exitedZero());
    BEZEL_EXPECT_TRUE(has(contra.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(contra.out, std::string("[ POISON ] ") + kCanary));
    BEZEL_EXPECT_TRUE(has(contra.out, "RESULTS CACHE: 0 reused, 1 executed, 0 stored, 1 poisoned"));
    BEZEL_EXPECT_TRUE(has(readAll(f.cacheDir + "/poison.log"), kCanary));
    dumpOnFailure(contra, "contradiction");

    // Still armed: the poisoned record is NOT served, so the body runs (FAIL).
    Leg after = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_FALSE(after.exitedZero());
    BEZEL_EXPECT_TRUE(has(after.out, "canary body executed"));
    BEZEL_EXPECT_FALSE(has(after.out, "[ CACHED ]"));
    dumpOnFailure(after, "after-poison");

    // Passing again does not un-poison it: executed, not stored, not served.
    f.arm(false);
    for (int i = 0; i < 2; ++i) {
        Leg again = f.run(kCanaryRow, "1");
        BEZEL_EXPECT_TRUE(again.exitedZero());
        BEZEL_EXPECT_TRUE(has(again.out, "canary body executed"));
        BEZEL_EXPECT_TRUE(has(again.out, "RESULTS CACHE: 0 reused, 1 executed, 0 stored, 0 poisoned"));
        dumpOnFailure(again, "re-pass");
    }
}

// ---------------------------------------------------------------------------
// The key is bound to the binary's identity, to the behaviour-changing
// environment, and the cache is off under BEZEL_AOT_FORCE_SANITIZE.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, key_binds_binary_env_and_sanitizer) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-key");

    Leg seed = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_TRUE(seed.exitedZero());
    BEZEL_EXPECT_TRUE(has(seed.out, "1 stored"));
    f.arm(true);

    // A behaviour knob flipped: a different key, so a miss (executed, FAIL).
    Leg env = f.run(kCanaryRow, "1", true, {{"BEZEL_AOT_DETECT_LEAKS", "report"}});
    BEZEL_EXPECT_FALSE(env.exitedZero());
    BEZEL_EXPECT_TRUE(has(env.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(env.out, "RESULTS CACHE: 0 reused, 1 executed"));
    dumpOnFailure(env, "env-flip");

    // Generated-code ASan forced: the cache is off.
    Leg san = f.run(kCanaryRow, "1", true, {{"BEZEL_AOT_FORCE_SANITIZE", "address"}});
    BEZEL_EXPECT_FALSE(san.exitedZero());
    BEZEL_EXPECT_TRUE(has(san.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(san.out, "(off: BEZEL_AOT_FORCE_SANITIZE"));
    dumpOnFailure(san, "force-sanitize");

    // A key file computed for another binary (another stat id): ignored.
    f.writeKeyFile("0:0:0:0.000000000");
    Leg other = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_FALSE(other.exitedZero());
    BEZEL_EXPECT_TRUE(has(other.out, "canary body executed"));
    BEZEL_EXPECT_TRUE(has(other.out, "(off: this binary is not in the key file"));
    dumpOnFailure(other, "other-binary");

    // Control: with the original key file and env the record is still served,
    // so the misses above were the key's doing, not a lost entry.
    f.writeKeyFile(rc::statId("/proc/self/exe"));
    Leg control = f.run(kCanaryRow, "1");
    BEZEL_EXPECT_TRUE(control.exitedZero());
    BEZEL_EXPECT_TRUE(has(control.out, std::string("[ CACHED ] ") + kCanary));
    dumpOnFailure(control, "control");
}

// ---------------------------------------------------------------------------
// A PASS that moved a loudness counter is not stored, so it re-runs (and
// re-prints its NATIVE SANITIZER skip) every time.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, loud_pass_is_not_stored) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-loud");
    for (int i = 0; i < 2; ++i) {
        Leg l = f.run(kLoudRow, "1");
        BEZEL_EXPECT_TRUE(l.exitedZero());
        BEZEL_EXPECT_TRUE(has(l.out, "NATIVE SANITIZER: 1 check(s) skipped"));
        BEZEL_EXPECT_TRUE(has(l.out, "RESULTS CACHE: 0 reused, 1 executed, 0 stored, 0 poisoned, 1 unstored"));
        dumpOnFailure(l, "loud");
    }
    BEZEL_EXPECT_EQ(f.passEntries(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// The identity probe scripts/results_cache.sh uses: prints the schema and this
// binary's stat id, and runs no test.
// ---------------------------------------------------------------------------
BEZEL_TEST(support_results_cache, describe_prints_identity_and_runs_nothing) {
    if (const char* c = std::getenv(kChildGuard); c && *c) return;
    Fixture f("rcache-describe");
    Leg d = f.run("__results_cache_describe_matches_nothing__", nullptr, false,
                  {{"BEZEL_RESULTS_CACHE_DESCRIBE", "1"},
                   {"BEZEL_TEST_ENUMERATE", "1"}});
    BEZEL_EXPECT_TRUE(d.exitedZero());
    BEZEL_EXPECT_TRUE(has(d.out, std::string("schema=") + rc::kSchema + "\n"));
    BEZEL_EXPECT_TRUE(has(d.out, "exe_stat=" + rc::statId("/proc/self/exe") + "\n"));
    BEZEL_EXPECT_TRUE(has(d.out, "repo_root=/"));
    BEZEL_EXPECT_TRUE(has(d.out, "runtime_lib=/"));
    BEZEL_EXPECT_FALSE(has(d.out, "[ RUN"));
    dumpOnFailure(d, "describe");
}
