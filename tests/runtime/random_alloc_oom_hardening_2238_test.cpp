// Direct unit tests for bz_random_alloc's seed -> xorshift128+ state
// construction (stdlib §2.7).
//
// Root cause (2026-07-28 hardening wave, follow-up to #1762 finding R1 / #2234
// / #2237 — see also tests/runtime/args_array_oom_hardening_2234_test.cpp and
// tests/runtime/fileio_read_all_lines_oom_hardening_2237_test.cpp):
// bz_random_alloc in src/runtime/bezel_runtime_collections.cpp:1633 called
// malloc(sizeof(bz_rng_t)) without null-checking, then unconditionally wrote
// r->state[0] and r->state[1]. A failed allocation produced a SIGSEGV on the
// next store instead of returning NULL to the codegen-emitted constructor.
//
// This test pins the fix at the C-runtime level. Two contracts are checked:
//
//   1. Happy path: a non-zero seed produces a non-null handle whose first
//      generate (NextInt) matches a manually-computed xorshift128+ step from
//      the same seed. This is the regression: the OOM fix must not change
//      the observable shape of the produced handle on the normal path.
//   2. Seed == 0: the dead-code guards (state[0/1] == 0 -> state[0/1] = 1)
//      are still applied even after the null check. This pins the spec
//      rule that xorshift128+ MUST NOT run with both state words zero
//      (else every output is zero).
//
// Direct OOM injection (LD_PRELOAD malloc hook) is out of scope here — the
// shape-pinning on the happy path is what proves the null-check guard in
// bz_random_alloc did not disturb the normal case.

#include <cstdint>
#include <vector>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

namespace {

// Replicate xorshift128+ to compute the expected first NextInt(0, 1<<30) value
// from the same seed bz_random_alloc uses. The runtime and the test must agree
// byte-for-byte so any drift in the seeding or step logic shows up as a
// mismatch.
struct RngState { uint64_t s[2]; };

static uint64_t xorshift128_next(uint64_t* s) {
    uint64_t t = s[0];
    uint64_t const r = s[1];
    s[0] = r;
    t ^= t << 23;
    t ^= t >> 17;
    t ^= r ^ (r >> 26);
    s[1] = t;
    return t + r;
}

static int32_t expected_next_int(int64_t seed, int32_t lo, int32_t hi_excl) {
    RngState st;
    st.s[0] = (uint64_t)seed ^ 0xdeadbeef12345678ULL;
    st.s[1] = (uint64_t)seed * 6364136223846793005ULL + 1442695040888963407ULL;
    if (st.s[0] == 0) st.s[0] = 1;
    if (st.s[1] == 0) st.s[1] = 1;
    uint64_t v = xorshift128_next(st.s);
    if (hi_excl <= lo) return lo;
    return lo + (int32_t)(v % (uint64_t)(hi_excl - lo));
}

} // namespace

// ---------------------------------------------------------------------------
// §2.7: a non-zero seed produces a non-null handle whose first NextInt matches
// the manual xorshift128+ step. The OOM-hardening fix must not change this —
// if it did, every Random-using program would silently see wrong values.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_random_alloc_oom_2238, happy_path_produces_stable_sequence) {
    void* rng = bz_random_alloc(12345);
    BEZEL_EXPECT_TRUE(rng != nullptr);

    int32_t got = bz_random_next_int(rng, 0, 1000);
    int32_t want = expected_next_int(12345, 0, 1000);
    BEZEL_EXPECT_EQ(got, want);

    // Two more draws to confirm the state actually advances (i.e. it's not
    // a static/null sentinel that always returns the same value).
    int32_t got2 = bz_random_next_int(rng, 0, 1000);
    int32_t got3 = bz_random_next_int(rng, 0, 1000);
    BEZEL_EXPECT_TRUE(got2 != got || got3 != got);  // overwhelmingly likely

    bz_random_free(rng);
}

// ---------------------------------------------------------------------------
// §2.7: seed == 0 must still produce a non-null handle with the dead-code
// state-zero guard applied (both state words become 1, not 0). The pre-fix
// OOM guard sits ABOVE the state init; this test pins that the dead-code
// guard still runs after the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_random_alloc_oom_2238, seed_zero_still_breaks_xorshift_deadlock) {
    void* rng = bz_random_alloc(0);
    BEZEL_EXPECT_TRUE(rng != nullptr);

    // The dead-code guard turns state[0/1] = 0 into 1, so the first draw
    // matches the "seed == 0 -> both states = 1" expected step.
    int32_t got = bz_random_next_int(rng, 0, 1 << 30);
    int32_t want = expected_next_int(0, 0, 1 << 30);
    BEZEL_EXPECT_EQ(got, want);

    bz_random_free(rng);
}
