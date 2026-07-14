// Regression pin for the AOT harness's content-addressed link-cache
// (aot_support.hpp cachedCompile). A single gate compiles every AOT test 2-3x
// under the SAME compiler state (sharded pass, serial confirmation, flake/
// arbitration reruns); the cache serves a SUCCESSFUL link product for an
// unchanged input so those repeats collapse to one real compile. These tests
// pin the two properties the cache's soundness rests on:
//
//   1. KEY COMPOSITION. The key is SHA-256 over (compiler identity ‖ runtime-
//      library identity ‖ compile/link mode ‖ program source). So it is
//      deterministic for a fixed input, source-sensitive, and mode-sensitive
//      (a plain, an --sanitize=address, and a --checked-disposal build of the
//      same source never collide). Verified here PURELY (no compile) against
//      computeCacheKey.
//   2. END-TO-END miss→hit→miss. A never-before-seen program compiles for real
//      (a cold MISS) the first time and is SERVED from the cache (a HIT) the
//      second, with identical observable behaviour; flipping the source bytes
//      misses again — exactly the changed program, nothing else. Verified via
//      the per-process hit/miss counters.
//
// Fail-without / pass-with: with the cache wired in, run #2 of an unchanged
// program is a HIT (no new miss); if cachedCompile were reverted to a plain
// forkedCompile, run #2 would compile again and the hit assertion would fail.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::codegen::CodegenOptions;
namespace aot = bezel::test::aot_detail;

namespace {

// A unique-per-process nonce so a program compiled by THIS test is guaranteed
// never to have been cached by a PRIOR suite run (a different pid): the "first
// compile" is then always a cold MISS regardless of what is already on disk.
std::uint64_t nextNonce() {
    static std::uint64_t n =
        (static_cast<std::uint64_t>(::getpid()) << 20) ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    return n++;
}

// A valid program whose source bytes embed `tag` (so distinct tags key
// distinctly) and whose Main returns `ret` (so a served binary is behaviourally
// checkable via the exit code).
std::string prog(const std::string& tag, int ret) {
    return "module M; /* " + tag +
           " */ class C public static method Main(): int "
           "  return " +
           std::to_string(ret) + "; end Main; end C;";
}

// True when the cache is actually active on this host (enabled AND a sound
// compiler identity is hashable). When it is not, the counter/key assertions
// below are vacuous, so the tests short-circuit.
bool cacheActive() {
    return aot::cacheEnabled() &&
           !aot::computeCacheKey("probe", CodegenOptions{}).empty();
}

}  // namespace

// Property 1a: the key is deterministic and source-sensitive.
BEZEL_TEST(aot_cache, key_is_stable_and_source_sensitive) {
    if (!cacheActive()) return;
    CodegenOptions plain;
    const std::string a = prog("A", 0);
    const std::string b = prog("B", 0);
    // Same source + same mode -> same key (a stale hit is impossible only if the
    // key is a pure function of its inputs).
    BEZEL_EXPECT_EQ(aot::computeCacheKey(a, plain),
                    aot::computeCacheKey(a, plain));
    // Different source -> different key.
    BEZEL_EXPECT_TRUE(aot::computeCacheKey(a, plain) !=
                      aot::computeCacheKey(b, plain));
}

// Property 1b: the key separates every compile/link mode — a plain, an ASan,
// and a checked-disposal build of the same source are distinct artifacts.
BEZEL_TEST(aot_cache, key_is_mode_sensitive) {
    if (!cacheActive()) return;
    const std::string a = prog("A", 0);
    CodegenOptions plain;
    CodegenOptions asan;
    asan.sanitizeAddress = true;
    CodegenOptions disp;
    disp.checkedDisposal = true;
    const std::string kp = aot::computeCacheKey(a, plain);
    BEZEL_EXPECT_TRUE(kp != aot::computeCacheKey(a, asan));
    BEZEL_EXPECT_TRUE(kp != aot::computeCacheKey(a, disp));
    BEZEL_EXPECT_TRUE(aot::computeCacheKey(a, asan) !=
                      aot::computeCacheKey(a, disp));
}

// Property 2: end-to-end miss -> hit -> (flip source) miss, with identical
// observable behaviour on the hit. Uses the per-process hit/miss counters; the
// suite runs tests serially within a shard process, so the deltas are exact.
BEZEL_TEST(aot_cache, unchanged_program_hits_and_flipped_source_misses) {
    if (!cacheActive()) return;

    auto& st = aot::cacheStats();
    const std::string a = prog("cache_e2e_" + std::to_string(nextNonce()), 7);

    // 1) A never-before-seen program compiles for real: a cold MISS.
    const std::uint64_t m0 = st.misses.load(), h0 = st.hits.load();
    JitResult r1 = runAot(a);
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 7);
    BEZEL_EXPECT_EQ(st.misses.load(), m0 + 1);  // missed (real compile)
    BEZEL_EXPECT_EQ(st.hits.load(), h0);        // did not hit

    // 2) The SAME program again is served from the cache: a HIT, no new compile,
    //    behaviourally identical.
    const std::uint64_t m1 = st.misses.load(), h1 = st.hits.load();
    JitResult r2 = runAot(a);
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 7);            // identical observable result
    BEZEL_EXPECT_EQ(st.hits.load(), h1 + 1);    // hit
    BEZEL_EXPECT_EQ(st.misses.load(), m1);      // no new miss

    // 3) FLIP the source (different bytes) -> exactly this program misses again;
    //    the previously-cached one is untouched. Proves source-addressing.
    const std::string b = prog("cache_e2e_" + std::to_string(nextNonce()), 9);
    const std::uint64_t m2 = st.misses.load(), h2 = st.hits.load();
    JitResult r3 = runAot(b);
    BEZEL_EXPECT_TRUE(r3.ran);
    BEZEL_EXPECT_EQ(r3.exitCode, 9);
    BEZEL_EXPECT_EQ(st.misses.load(), m2 + 1);  // flipped source -> missed
    BEZEL_EXPECT_EQ(st.hits.load(), h2);        // did not hit
}
