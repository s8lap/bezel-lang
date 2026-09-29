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
//   2. END-TO-END compile→hit→compile. A never-before-seen program runs the
//      front-end for real the first time and is SERVED WHOLE from the cache the
//      second, with identical observable behaviour; flipping the source bytes
//      compiles again — exactly the changed program, nothing else. Verified via
//      the per-process level counters (see the restatement note on that test:
//      under the two-level split the observable is exeKeyHits/objHits, not the
//      coarser hits/misses).
//
// Fail-without / pass-with: with the cache wired in, run #2 of an unchanged
// program is a whole-build HIT (no compile, no link); if cachedCompile were
// reverted to a plain forkedCompile, run #2 would compile again and the hit
// assertion would fail.
//
// The KEYS of the two-level split — which input must invalidate which half —
// are pinned separately in aot_cache_key_test.cpp.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "support/build_paths.hpp"  // #5010: the BEZEL_*_PATH / BEZELC_PATH aliases
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

// Property 1c (#2100 k2 refinement): the key schema is "k2;" and the compiler
// archive the refined identity hashes is actually wired in. Fail-without /
// pass-with: reverting the identity refinement without bumping the schema tag
// back would leave "k2;" false here ("k1;" reappears); dropping the
// BEZEL_CORE_LIB_PATH definition from tests/CMakeLists.txt (which would
// silently degrade every gate to the coarse exe-hash identity, i.e. cold on
// every relink) fails the wiring assertions.
BEZEL_TEST(aot_cache, key_schema_is_k2_and_core_lib_wired) {
    CodegenOptions plain;
    const std::string mode = aot::modeString(plain);
    // The schema tag must be "k2;…" — a k1 entry (whole-binary identity) must
    // never be served under a k2 key or vice versa.
    BEZEL_EXPECT_TRUE(mode.rfind("k2;", 0) == 0);
#ifdef BEZEL_CORE_LIB_PATH
    // The refined identity's input exists on disk for this build.
    BEZEL_EXPECT_TRUE(std::filesystem::exists(BEZEL_CORE_LIB_PATH));
#else
    // The definition is load-bearing for the k2 refinement; its absence is a
    // build-wiring regression (tests/CMakeLists.txt), not an environment fact.
    BEZEL_EXPECT_TRUE(false);
#endif
}

// Property 2: end-to-end miss -> hit -> (flip source) miss, with identical
// observable behaviour on the hit. Uses the per-process hit/miss counters; the
// suite runs tests serially within a shard process, so the deltas are exact.
//
// RESTATED FOR THE TWO-LEVEL ("k3") SPLIT. The property is unchanged; the
// COUNTER that observes it had to become more precise. `misses` used to mean
// "the compiler ran", because a build was one indivisible step. It now means
// only "the LINKER ran" — and a never-before-seen program need not reach the
// linker: its source is new, so the front-end always runs, but if that
// front-end emits an object byte-identical to one already linked (these
// programs differ only inside a `/* … */` comment, which produces no machine
// code), the link key hits and the identical executable is served. That is the
// content-addressing working as designed, not a stale hit: same object + same
// runtime + same recipe is the same executable by construction.
//
// So each step now asserts on the level it is actually about:
//   * "the front-end ran"       -> objHits did NOT move
//   * "the whole build was served" -> exeKeyHits DID move
// which is strictly sharper than the old pair (it distinguishes a whole-build
// hit from a link-only hit, where `hits` alone could not).
BEZEL_TEST(aot_cache, unchanged_program_hits_and_flipped_source_misses) {
    if (!cacheActive()) return;

    auto& st = aot::cacheStats();
    const std::string a = prog("cache_e2e_" + std::to_string(nextNonce()), 7);

    // 1) A never-before-seen program runs the FRONT-END for real: no object was
    //    cached for this source, so nothing about it can have been served.
    const std::uint64_t o0 = st.objHits.load(), e0 = st.exeKeyHits.load();
    JitResult r1 = runAot(a);
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 7);
    BEZEL_EXPECT_EQ(st.objHits.load(), o0);     // compiled (no object served)
    BEZEL_EXPECT_EQ(st.exeKeyHits.load(), e0);  // no whole-build hit

    // 2) The SAME program again is served WHOLE from the cache: no compile, no
    //    link, behaviourally identical.
    const std::uint64_t m1 = st.misses.load(), e1 = st.exeKeyHits.load(),
                        o1 = st.objHits.load();
    JitResult r2 = runAot(a);
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 7);              // identical observable result
    BEZEL_EXPECT_EQ(st.exeKeyHits.load(), e1 + 1);  // served whole
    BEZEL_EXPECT_EQ(st.misses.load(), m1);        // linker did not run
    BEZEL_EXPECT_EQ(st.objHits.load(), o1);       // front-end did not run either

    // 3) FLIP the source (different bytes) -> exactly this program compiles
    //    again and is NOT served the previous program's build; the previously
    //    cached one is untouched. Proves source-addressing. The exit code is the
    //    load-bearing half: a stale hit would return 7 here, not 9.
    const std::string b = prog("cache_e2e_" + std::to_string(nextNonce()), 9);
    const std::uint64_t o2 = st.objHits.load(), e2 = st.exeKeyHits.load();
    JitResult r3 = runAot(b);
    BEZEL_EXPECT_TRUE(r3.ran);
    BEZEL_EXPECT_EQ(r3.exitCode, 9);
    BEZEL_EXPECT_EQ(st.objHits.load(), o2);     // flipped source -> compiled
    BEZEL_EXPECT_EQ(st.exeKeyHits.load(), e2);  // no whole-build hit
}
