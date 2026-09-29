// Direct unit tests for the capture-cell over-release guard in
// src/runtime/bezel_runtime_refcounting.cpp (bz_cell_retain / bz_cell_release),
// spec §27.5 + §28.9 (v0.306) capture cells, fix #4564.
//
// THE BUG (fix #4564). #4547 hardened the three release functions whose
// last-drop guard tested the POST-decrement count with `> 0`: an over-release
// drove that value to -1, which is not `> 0`, so it fell into the same free()
// path a genuine last drop takes. `bz_cell_release` (added by #4509, same
// file) reaches the SAME double free by a different route, so #4547's
// three-way `> 0` rewrite does not transfer — there is no count left to go
// negative. `bz_cell_release` keeps its count in a mutex-guarded SIDE TABLE
// and, before this fix, ERASED the entry at the last drop:
//
//   auto it = g_cell_rc.find(cell);
//   if (it != g_cell_rc.end()) {
//       if (--it->second > 0) return;
//       g_cell_rc.erase(it);            // <-- the information is lost here
//   }
//   // "not in the table: the frame is the only holder"
//   free(cell);
//
// After the erase, a release PAST the last drop finds no entry and is
// indistinguishable from the first release of a cell no env ever retained — so
// it takes the sole-owner arm and calls free() on a block this same function
// freed one call earlier. A double free, from an over-release that was a
// single detectable bad release one call before.
//
// THE FIX. The last drop leaves a TOMBSTONE (the entry stays, count at
// BZ_CELL_TOMBSTONE) instead of erasing, and a release that finds a tombstone
// is reported by #4547's bz_refcount_overrelease_fail before any free. Two
// reclaim routes keep that from becoming a leak of its own: a retain landing on
// a tombstone re-arms the entry (the address has been recycled by calloc into a
// NEW cell), and a bounded ring evicts the oldest tombstone so the table cannot
// grow without bound on a correct program. See the block comment at
// g_cell_rc's declaration for the full argument.
//
// ORACLE. A forked child (process death is the expected outcome of the repro)
// with stderr captured via a pipe, mirroring
// refcount_overrelease_4547_test.cpp's shape:
//   * PASS (with the fix)    -> SIGABRT and stderr carries the SPECIFIC
//                               "refcount over-release (capture cell)"
//                               diagnostic.
//   * FAIL-WITHOUT (pre-fix) -> also SIGABRT, but from glibc's "free(): double
//                               free detected" (or ASan's "attempting
//                               double-free") — our diagnostic is absent.
// The signal alone therefore does NOT discriminate here and is not the oracle;
// the diagnostic TEXT is, exactly as CLAUDE.md's crash-oracle guidance
// requires. `bz_cell_release` never dereferences the cell (it uses the pointer
// as a table key and frees it), so the repro can use a plain 8-byte block —
// the size #4509 measures at 8 B per capture.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

namespace {

constexpr std::size_t kCellSize = 8;  // #4509's measured cell: 8 B per capture

struct ChildResult {
    bool signaled = false;
    int termsig = 0;
    bool exited = false;
    int exitCode = -1;
    std::string stderrOutput;
};

// Run `body` in a forked child with its stderr captured via a pipe. Same
// helper shape as refcount_overrelease_4547_test.cpp's runChild: capturing the
// TEXT (not just the signal) is what separates FAIL-WITHOUT (an abort from the
// allocator) from PASS-WITH (an abort from our own guard).
template <typename F>
ChildResult runChild(F body) {
    ChildResult result;
    int pipefd[2];
    if (pipe(pipefd) != 0) return result;  // neither signaled nor exited -> fails any assertion
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        body();
        _exit(0);
    }
    close(pipefd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        result.stderrOutput.append(buf, (std::size_t)n);
    close(pipefd[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return result;
    if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.termsig = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        result.exited = true;
        result.exitCode = WEXITSTATUS(status);
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// #4564 / §27.5 + §28.9 v0.306 — THE REPRO. The issue's own sequence, in the
// order codegen emits it: one env announces itself (`bz_cell_retain`, count =
// frame + this env = 2), the env's envdtor lets go, the declaring scope lets
// go (count 0 -> the cell's one legitimate free), and then one release too
// many arrives. Pre-fix that last call found an erased table, took the
// "sole owner" arm, and free()d the block a second time.
//
// This is a genuine over-release, not a constructed count: every call below is
// a real call on a real cell, in sequence. It does not need #4547's
// count-pre-set trick, because the state that makes the defect reachable here
// is the table's, not a word inside a block this function is about to free —
// no invalid memory is touched on EITHER side of the fix up to the point of
// failure, so the row is deterministic under both the plain and the --full
// (ASan) oracle.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_cell_release_overrelease_4564,
           release_past_the_last_drop_aborts_loudly_spec_27_5) {
    ChildResult r = runChild([] {
        void* cell = std::calloc(1, kCellSize);
        bz_cell_retain(cell);   // frame + this env == 2
        bz_cell_release(cell);  // the env's envdtor lets go     -> 1
        bz_cell_release(cell);  // the declaring scope lets go   -> 0, freed
        bz_cell_release(cell);  // ONE TOO MANY -> pre-fix: free() again
    });
    BEZEL_EXPECT_TRUE(r.signaled);
    BEZEL_EXPECT_TRUE(r.termsig == SIGABRT);
    // The discriminating assertion: pre-fix the abort comes from the allocator
    // ("free(): double free detected in tcache 2" / ASan's "attempting
    // double-free") and carries neither of these substrings.
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("capture cell") != std::string::npos);
}

// CONTROL — the ordinary two-holder lifecycle must be untouched. Without this
// row a "fix" that aborted on every release, or that stopped freeing at the
// last drop, would still pass the repro above.
BEZEL_TEST(runtime_cell_release_overrelease_4564,
           normal_two_holder_last_drop_frees_cleanly_spec_27_5) {
    ChildResult r = runChild([] {
        void* cell = std::calloc(1, kCellSize);
        bz_cell_retain(cell);   // frame + this env == 2
        bz_cell_release(cell);  // -> 1, still alive
        bz_cell_release(cell);  // -> 0, the one legitimate free
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}

// CONTROL — the UNTRACKED arm, which the tombstone must not disturb. A cell no
// env ever retained never enters the table, and its single release is a plain
// free (the pre-#4509 behaviour the two protocols coexist on). A tombstone
// implementation that reported on "entry absent" rather than on "entry present
// and dead" would abort here.
BEZEL_TEST(runtime_cell_release_overrelease_4564,
           untracked_cell_release_is_still_a_plain_free_spec_27_5) {
    ChildResult r = runChild([] {
        void* cell = std::calloc(1, kCellSize);
        bz_cell_release(cell);  // never retained: the frame is the sole holder
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}

// ---------------------------------------------------------------------------
// #4564 / §27.5 + §28.9 — THE RECYCLED-ADDRESS ROW, and the reason a tombstone
// needs route (1). A cell's block is freed at its last drop and the next cell
// lands on the same address constantly — measured 197 of 200 iterations on
// #4509's own event-subscription loop under glibc. A retain therefore arrives
// at a TOMBSTONED entry all the time, and it must RE-ARM the entry at 2
// (frame + this env), not increment off the tombstone.
//
// The loop below fails loudly if it did increment: the count after the retain
// would be 1, so the env's release would drive it to 0 and FREE the cell while
// the frame still holds it — and the very next line writes through that
// pointer, which is a heap-use-after-free (ASan faults at the write under the
// --full oracle; the plain build then double-frees on the following release
// and aborts in the allocator). Either way the child dies and this row fails.
//
// 500 iterations, per CLAUDE.md's loop-amplification rule: the row must not
// depend on a single iteration happening to recycle an address. Under the
// plain oracle glibc's tcache recycles on essentially every iteration; under
// ASan's quarantine it recycles rarely, and the row degrades to a second
// 500-iteration control on the ordinary lifecycle. The observed recycle count
// is printed to stderr so the log carries the number rather than leaving it to
// be assumed; the assertion is on the count's PRESENCE, not its value, because
// the value is an allocator property and would disagree between the oracles.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_cell_release_overrelease_4564,
           retain_on_a_recycled_address_rearms_the_count_spec_27_5) {
    ChildResult r = runChild([] {
        void* previous = nullptr;
        long recycled = 0;
        for (int i = 0; i < 500; ++i) {
            void* cell = std::calloc(1, kCellSize);
            if (cell == previous) ++recycled;
            previous = cell;
            bz_cell_retain(cell);   // frame + this env == 2 (re-armed if tombstoned)
            bz_cell_release(cell);  // the env lets go -> 1; the cell MUST be alive
            // The frame still holds it: this write is valid only if the retain
            // re-armed the count. It is the use-after-free that fails the row.
            std::memcpy(cell, &i, sizeof(int));
            bz_cell_release(cell);  // the frame lets go -> 0, freed exactly once
        }
        std::fprintf(stderr, "cell-reuse=%ld\n", recycled);
        std::fflush(stderr);
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("cell-reuse=") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}
