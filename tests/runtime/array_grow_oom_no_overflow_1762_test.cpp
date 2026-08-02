// Regression tests for the allocation-failure path of the collection runtime
// (src/runtime/bezel_runtime_collections.cpp), spec §31.4 + stdlib §4.4/§4.5.
//
// Spec §31.4 "Allocation failure" (normative, v0.226) names collection growth
// explicitly:
//
//   "When a runtime allocation cannot obtain memory — a `new` expression,
//    collection growth, string construction, boxing, or the allocation of a
//    closure, iterator, or async state-machine frame — the program TERMINATES:
//    the runtime reports the failure and aborts the process. Allocation failure
//    is not an exception. … The report's form is implementation-defined; the
//    termination is not."
//
// Two earlier postures both violated it, and both were memory-unsafe:
//
//   1. PRE-#1762: `arr->data = realloc(arr->data, n)` — on failure `data`
//      became NULL, the original block leaked, and the immediately following
//      memcpy dereferenced NULL. Terminates, but by SIGSEGV, with no report,
//      and only by luck (the write happens to land near offset 0).
//   2. #1762's first cut: keep the old buffer and return without growing. The
//      callers write unconditionally, so this wrote elem_size bytes past the
//      end of a buffer that was never grown — a silent HEAP-BUFFER-OVERFLOW,
//      strictly worse than (1) because it corrupts unrelated heap instead of
//      faulting.
//
// The fix routes every allocation failure through `bz_alloc_fail`, which
// reports to stderr and abort()s — §31.4's mandated termination.
//
// Forcing a real allocation failure needs no malloc interposer: bz_array_grow
// doubles an empty array to capacity 4, so an element size of 2^50 makes the
// growth request 4 * 2^50 = 2^52 bytes (~4.5 PB). No allocator satisfies that,
// on any box, so the failure is deterministic and portable.
//
// Each case runs in a forked child, because the expected outcome is process
// death. FAIL-WITHOUT / PASS-WITH is discriminated by the SIGNAL:
//   * with the fix        -> SIGABRT (6), from bz_alloc_fail's abort()
//   * posture (1) above   -> SIGSEGV (11), from the NULL memcpy
//   * posture (2) above   -> no signal at all (silent overflow, child exits 0)
// so asserting specifically on SIGABRT fails under both broken postures.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

namespace {

// Element size whose first growth step (capacity 4) cannot be allocated.
constexpr int64_t kUnsatisfiableElemSize = (int64_t)1 << 50;

// Run `body` in a forked child; return true iff the child died by SIGABRT.
// Anything else — clean exit, SIGSEGV, any other signal — returns false.
template <typename F>
bool diesByAbort(F body) {
    pid_t pid = fork();
    if (pid < 0) return false;  // cannot fork: report as failure, never a pass
    if (pid == 0) {
        // Child. Silence the expected stderr report so the test log stays
        // readable, then run the body. If the body RETURNS (no termination),
        // exit 0 — which the parent treats as a failure.
        (void)freopen("/dev/null", "w", stderr);
        body();
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

}  // namespace

// ---------------------------------------------------------------------------
// §31.4 + stdlib §4.5 Add: a growth that cannot be satisfied terminates the
// process. It must NOT return quietly (which would write past the un-grown
// buffer) and must NOT fault on a NULL pointer.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_array_grow_oom_1762, add_on_failed_growth_terminates) {
    BEZEL_EXPECT_TRUE(diesByAbort([] {
        void* arr = bz_array_alloc(kUnsatisfiableElemSize, 0);
        unsigned char src[64];
        std::memset(src, 0xAB, sizeof(src));
        bz_array_add(arr, src, kUnsatisfiableElemSize);
    }));
}

// ---------------------------------------------------------------------------
// §31.4: the insert-at path has its own grow call site and its own
// memmove+memcpy pair, so it needs its own coverage.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_array_grow_oom_1762, insert_at_on_failed_growth_terminates) {
    BEZEL_EXPECT_TRUE(diesByAbort([] {
        void* arr = bz_array_alloc(kUnsatisfiableElemSize, 0);
        unsigned char src[64];
        std::memset(src, 0xCD, sizeof(src));
        bz_array_insert_at(arr, 0, src, kUnsatisfiableElemSize);
    }));
}

// ---------------------------------------------------------------------------
// §31.4: an unsatisfiable Reserve is still a failed collection growth, so it
// terminates too. (Reserve writes nothing, so this one was not memory-unsafe
// before — but §31.4 draws no distinction, and silently not-reserving would
// leave the program running on a promise the runtime did not keep.)
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_array_grow_oom_1762, reserve_on_failed_growth_terminates) {
    BEZEL_EXPECT_TRUE(diesByAbort([] {
        void* arr = bz_array_alloc((int64_t)sizeof(int64_t), 0);
        bz_array_reserve(arr, (int64_t)sizeof(int64_t), (int64_t)1 << 50);
    }));
}

// ---------------------------------------------------------------------------
// §31.4: the Array<T> constructor's own backing-store allocation. Pre-fix this
// set `capacity` whether or not the buffer materialized, handing back a header
// that claimed capacity over a NULL pointer.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_array_grow_oom_1762, alloc_with_unsatisfiable_capacity_terminates) {
    BEZEL_EXPECT_TRUE(diesByAbort([] {
        (void)bz_array_alloc((int64_t)sizeof(int64_t), (int64_t)1 << 50);
    }));
}

// ---------------------------------------------------------------------------
// CONTROL (stdlib §4.5): the abort path must not disturb the normal path — a
// satisfiable growth still stores every element and reads them all back in
// order. Without this control, a "fix" that aborted on EVERY growth would pass
// all four tests above.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_array_grow_oom_1762, normal_growth_still_stores_every_element) {
    void* arr = bz_array_alloc((int64_t)sizeof(int64_t), 0);
    BEZEL_EXPECT_TRUE(arr != nullptr);

    // Enough elements to force several doublings (0 -> 4 -> 8 -> 16 -> 32).
    for (int64_t i = 0; i < 25; ++i)
        bz_array_add(arr, &i, (int64_t)sizeof(int64_t));

    BEZEL_EXPECT_EQ(bz_array_len(arr), (int64_t)25);
    BEZEL_EXPECT_TRUE(bz_array_capacity(arr) >= 25);

    for (int64_t i = 0; i < 25; ++i) {
        int64_t out = -1;
        bz_array_get(arr, i, &out, (int64_t)sizeof(int64_t));
        BEZEL_EXPECT_EQ(out, i);
    }

    bz_array_free(arr, nullptr);
}
