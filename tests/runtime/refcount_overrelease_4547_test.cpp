// Direct unit tests for the refcount over-release guard in
// src/runtime/bezel_runtime_refcounting.cpp (bz_string_release / bz_closure_release
// / bz_box_release), spec §28.9 (v0.306) runtime-managed types, fix #4547.
//
// THE BUG (fix #4547). Before this fix, the release guard on all three of
// bz_string_release / bz_closure_release / bz_box_release read:
//
//   if (__atomic_sub_fetch(rcp, 1, __ATOMIC_ACQ_REL) > 0)
//       return;
//   // "last reference gone" — free.
//
// An OVER-RELEASE (two releases issued for one reference — always a defect at
// the call site, never in this guard) drives the DECREMENTED value to -1,
// which is not `> 0`, so execution fell through to the SAME last-drop free()
// path as a genuine last release. Measured in #4540: two threads raced to
// release one closure; the loser's decrement produced -1 and both callers
// freed the same block — a double free that, one release earlier, was a
// single detectable bad release.
//
// WHY THESE TESTS CONSTRUCT THE PRECONDITION DIRECTLY, RATHER THAN REPLAYING
// #4540's RACE. A literal two-real-release replay (retain to 1, release once
// legitimately, release again on the same now-freed pointer) makes the SECOND
// call's atomic op a genuine heap-use-after-free on memory libc has already
// reclaimed: whether that corrupts, aborts, or silently "succeeds" then
// depends on glibc's tcache layout (which bytes of the freed chunk the
// allocator's own free-list bookkeeping happens to overwrite) — allocator-
// version-dependent and exactly the kind of environment-dependent flake
// CLAUDE.md's oracle catalogue warns against, and under ASan a real UAF is
// intercepted by ASan's OWN report before this guard ever runs, which would
// make a "does the diagnostic say X" assertion disagree between the plain and
// `--full` oracles. Instead, each test below constructs a FRESH, NEVER-FREED
// block with the rc field pre-set to 0 — the issue's own framing of the
// defect ("On an over-release the count goes to -1") — and releases it once.
// The guard cannot distinguish "this 0 came from one real prior release" from
// "this 0 was placed here directly": both are the same bit pattern at the
// point the guard reads it, so this exercises exactly the branch the fix
// changed, deterministically, with no invalid memory access on EITHER side of
// the fix (pre-fix, the single release here still frees only ONE valid,
// still-allocated block, so the test is well-defined and clean under ASan
// too — it just does not abort, which is the FAIL-WITHOUT signature below).
//
// ORACLE. A forked child (process death is the expected outcome) with stderr
// captured via a pipe:
//   * PASS (with the fix)    -> SIGABRT, stderr contains the specific
//                               "refcount over-release" diagnostic naming the
//                               type (bz_refcount_overrelease_fail).
//   * FAIL-WITHOUT (pre-fix) -> clean exit(0), no diagnostic at all — the
//                               over-release silently completes a free() as
//                               if it were a legitimate last release.
// Asserting the SPECIFIC diagnostic text (not just "some signal fired") is
// deliberate per CLAUDE.md's crash-oracle guidance: a bare SIGABRT could also
// come from an unrelated crash and would not discriminate FAIL-WITHOUT from
// PASS-WITH.
//
// A companion CONTROL test per type asserts the fix does not disturb the
// ordinary last-drop path: rc initialized to 1 (the real producer state),
// released once, must free cleanly with no abort and no diagnostic — without
// this control, a "fix" that aborted on every release would pass every test
// above.

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

// Byte offsets/sizes mirrored from src/runtime/bezel_runtime_refcounting.cpp's
// (file-local) BZ_*_RC_OFFSET constants and the layout comments above each
// release function — not exported via the public runtime.h, so re-derived
// here the same way dict_alloc_oom_hardening_2239_test.cpp re-derives the
// bz_string header shape.
constexpr std::size_t kClosureCellSize = 48;  // fn,env,rc,envdtor,kind,disposed
constexpr std::size_t kClosureRcOffset = 16;
constexpr std::size_t kStringHeaderSize = 32;  // data,len,width(+pad),rc
constexpr std::size_t kStringRcOffset = 24;
constexpr std::size_t kBoxCellSize = 24;  // vtable,rc,+payload headroom
constexpr std::size_t kBoxRcOffset = 8;

void setRc(void* block, std::size_t offset, int64_t value) {
    std::memcpy((uint8_t*)block + offset, &value, sizeof(int64_t));
}

struct ChildResult {
    bool signaled = false;
    int termsig = 0;
    bool exited = false;
    int exitCode = -1;
    std::string stderrOutput;
};

// Run `body` in a forked child with its stderr captured via a pipe. Mirrors
// array_grow_oom_no_overflow_1762_test.cpp's diesByAbort, extended to capture
// the diagnostic TEXT (not just the signal) so FAIL-WITHOUT (no crash at all)
// is unambiguously distinguished from a coincidental unrelated abort.
template <typename F>
ChildResult runChild(F body) {
    ChildResult result;
    int pipefd[2];
    if (pipe(pipefd) != 0) return result;  // signaled/exited both false -> fails any assertion
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }
    if (pid == 0) {
        // Child: redirect stderr to the pipe's write end; run the body. If the
        // body RETURNS (no termination), exit 0 — treated by the caller as
        // "no crash, no diagnostic" (the FAIL-WITHOUT signature).
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
// fix #4547 / §28.9 v0.306: bz_closure_release on a count that is already 0
// (an over-release's defect condition, per the issue's own framing) must
// terminate loudly, naming itself, instead of silently free()ing as if the
// call were a genuine last release.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_refcount_overrelease_4547, closure_release_negative_count_aborts_loudly) {
    ChildResult r = runChild([] {
        void* cell = std::calloc(1, kClosureCellSize);  // fn=env=envdtor=0, kind=0, disposed=0
        setRc(cell, kClosureRcOffset, 0);                // pre-set to the over-release precondition
        bz_closure_release(cell);
    });
    BEZEL_EXPECT_TRUE(r.signaled);
    BEZEL_EXPECT_TRUE(r.termsig == SIGABRT);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("closure") != std::string::npos);
}

// CONTROL: the fix must not disturb the ordinary last-drop path.
BEZEL_TEST(runtime_refcount_overrelease_4547, closure_release_normal_last_drop_frees_cleanly) {
    ChildResult r = runChild([] {
        void* cell = std::calloc(1, kClosureCellSize);
        setRc(cell, kClosureRcOffset, 1);  // the real producer's starting count
        bz_closure_release(cell);          // the one legitimate release: rc 1 -> 0
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}

// ---------------------------------------------------------------------------
// fix #4547 / §28.9 v0.306: bz_string_release — the same guard shape, same
// fix, same family (this file's over-release helper is shared across all
// three sites named below).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_refcount_overrelease_4547, string_release_negative_count_aborts_loudly) {
    ChildResult r = runChild([] {
        void* hdr = std::calloc(1, kStringHeaderSize);  // data=null, len=0, width=0 (not the SENTINEL)
        setRc(hdr, kStringRcOffset, 0);
        bz_string_release(hdr);
    });
    BEZEL_EXPECT_TRUE(r.signaled);
    BEZEL_EXPECT_TRUE(r.termsig == SIGABRT);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("string") != std::string::npos);
}

BEZEL_TEST(runtime_refcount_overrelease_4547, string_release_normal_last_drop_frees_cleanly) {
    ChildResult r = runChild([] {
        void* hdr = std::calloc(1, kStringHeaderSize);
        setRc(hdr, kStringRcOffset, 1);
        bz_string_release(hdr);
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}

// ---------------------------------------------------------------------------
// fix #4547 / §28.9 v0.306: bz_box_release — same guard shape, same fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_refcount_overrelease_4547, box_release_negative_count_aborts_loudly) {
    ChildResult r = runChild([] {
        void* box = std::calloc(1, kBoxCellSize);  // vtable=null (box_release never derefs it)
        setRc(box, kBoxRcOffset, 0);
        bz_box_release(box);
    });
    BEZEL_EXPECT_TRUE(r.signaled);
    BEZEL_EXPECT_TRUE(r.termsig == SIGABRT);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("box") != std::string::npos);
}

BEZEL_TEST(runtime_refcount_overrelease_4547, box_release_normal_last_drop_frees_cleanly) {
    ChildResult r = runChild([] {
        void* box = std::calloc(1, kBoxCellSize);
        setRc(box, kBoxRcOffset, 1);
        bz_box_release(box);
    });
    BEZEL_EXPECT_TRUE(r.exited);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stderrOutput.find("refcount over-release") == std::string::npos);
}
