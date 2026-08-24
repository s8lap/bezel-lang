// Test scratch-directory policy — the ONE definition of where test scratch
// lives, and the ONE mechanism that cleans it up.
//
// WHY THIS IS ITS OWN HEADER. These helpers used to live in aot_support.hpp,
// which pulls driver/Driver.hpp and three LLVM headers. Only the 58 test TUs
// that actually drive runAot could afford that include; the other ~100 that
// merely need a scratch directory hardcoded their own path instead, and every
// one of them hardcoded it WITHOUT cleanup. Splitting the policy into a header
// whose only dependency is <filesystem> lets every test share the one
// definition. aot_support.hpp includes this and is otherwise unchanged.
//
// THE LEAK THIS CLOSES (2026-08-24). 159 test files created their own scratch
// directory as `fs::path("<hardcoded>") / unique_stem("wt-<tag>")`. None of
// them removed it, and reapStaleWorkDirs() could not: it scans scratchBase()
// and matches only names beginning with "aot", so a "wt-" dir under a
// hardcoded parent missed on both counts. They accumulated without bound —
// 148,949 dirs / 41 GB under /var/tmp/bezel-scratch, plus 2,643 dirs / 12 GB
// under /tmp from the four files that hardcoded the tmpfs. The /tmp pile
// exhausted the tmpfs user quota (12,774 MB) and every write to /tmp began
// failing with EDQUOT, which wedged the session harness itself for three
// sessions before the cause was found. Per CLAUDE.md scratch must never live
// in /tmp (tmpfs is RAM); wtScratchDir() makes that structural rather than a
// rule each test file has to remember.
//
// THE MECHANISM IS BELT AND BRACES, because a test process can die without
// running destructors:
//   belt   — wtScratchDir() registers each dir for removal at process exit.
//   braces — reapStaleWorkDirs() age-gate-sweeps whatever a SIGKILL left
//            behind (shard timeout, OOM cage), on the next process's first
//            use. wtScratchDir() calls it directly rather than relying on
//            makeWorkDir(): a binary that never drives runAot would
//            otherwise get the RAII half and no sweep at all. The age gate
//            (reapAgeSec) is what makes it safe under concurrent shards.
// BEZEL_AOT_KEEP disables both, for debugging.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "support/unique_stem.hpp"

namespace bezel::test {

namespace aot_detail {

namespace fs = std::filesystem;

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

// Base scratch dir for per-run temp artifacts — the per-test workdir holding the
// emitted object, the linked executable, and the program's own run.
//
// COLOCATED WITH THE CACHE, BY CONSTRUCTION. storeToCache() and
// installFromCache() both PREFER fs::create_hard_link and fall back to a full
// byte copy, and a hardlink only works WITHIN ONE FILESYSTEM. That invariant
// used to hold by accident — both paths were hardcoded siblings under
// /var/tmp/bezel-scratch — and then the bincache was pointed at a faster
// spindle (a symlink to /mnt/fast), which silently turned every store and every
// hit into a cross-device copy. Nothing failed; the harness just got slower,
// and the comment on installFromCache went on asserting "the cache and workdir
// share /var/tmp" for months. Deriving the workdir from the cache makes the
// invariant hold by construction instead of by two paths agreeing.
//
// Measured 2026-08-21, 1,266-test ASan slice, empty cache, conc=8, one variable:
//     workdir on /var/tmp (a 7200 RPM ST2000DM008): 211s, 5.2 GB in 3345 files
//     workdir beside the cache on NVMe            : 140s, 2.7 GB in 3345 files
// 34% faster, and HALF the bytes for the same file count — the exe-key and
// link-key entries are two hardlinks to one inode instead of two copies.
//
// When the cache dir is a plain directory this reproduces the historical path
// exactly, so a box without the symlink sees no change. Per CLAUDE.md the base
// must never be /tmp (tmpfs is RAM); the fallback keeps /var/tmp. An explicit
// BEZEL_AOT_SCRATCH_DIR overrides.
inline const std::string& scratchBase() {
    static const std::string* v = [] {
        if (const char* e = std::getenv("BEZEL_AOT_SCRATCH_DIR"); e && *e)
            return new std::string(e);
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(fs::path(cacheDir()), ec);
        if (!ec && c.has_parent_path() && !c.parent_path().empty())
            return new std::string((c.parent_path() / "aot-harness").string());
        return new std::string("/var/tmp/bezel-scratch/aot-harness");
    }();
    return *v;
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
            // Only our own dirs: the mkdtemp workdirs ("aotXXXXXX") and the
            // per-process test dirs ("wt-<tag>_<pid>_<n>", see wtScratchDir).
            // Never touch a sibling tool's scratch that may share the base.
            const std::string name = p.filename().string();
            if (name.rfind("aot", 0) != 0 && name.rfind("wt-", 0) != 0) continue;
            if (!fs::is_directory(p, ecEntry) || ecEntry) continue;
            const auto mt = fs::last_write_time(p, ecEntry);
            if (ecEntry) continue;
            if (now - mt > maxAge) fs::remove_all(p, ecEntry);  // best-effort
        }
        return true;
    }();
    (void)once;
}

}  // namespace aot_detail

}  // namespace bezel::test

namespace bezel_test {

// A per-process scratch directory named "wt-<tag>_<pid>_<counter>", created
// under the shared scratch base (never /tmp — see the file comment) and
// removed at process exit.
//
// MEMOISED PER TAG, which is what makes this a drop-in for the 159 call sites
// it replaces. Each of those was a function-local `static const fs::path dir`,
// i.e. exactly one directory per call site per process, shared by every test
// in the TU — tests routinely write a file under it in one test and read it
// back in another. A helper that minted a fresh directory per call would
// compile everywhere and break exactly those tests, so the memo is load-
// bearing, not an optimisation. The reference is into a std::map, whose nodes
// are address-stable across later insertions.
//
// The per-process stem (unique_stem: getpid + atomic counter) is what keeps
// concurrent shards from racing each other's files.
inline const std::filesystem::path& wtScratchDir(const char* tag) {
    namespace fs = std::filesystem;
    namespace detail = bezel::test::aot_detail;

    struct State {
        std::mutex m;
        std::map<std::string, fs::path> dirs;
        // belt: remove what this process created, on the normal exit path.
        ~State() {
            if (detail::keepArtifacts()) return;
            std::error_code ec;
            for (const auto& entry : dirs) fs::remove_all(entry.second, ec);
        }
    };
    static State state;

    // braces: sweep prior-run leaks once per process. This call is why the
    // guarantee is real for binaries that never touch runAot — the reaper
    // otherwise fires only from makeWorkDir(), so a process using wtScratchDir
    // alone would have had the RAII half and nothing else. Both halves are
    // needed: RAII cannot run when the shard is SIGKILLed (BEZEL_SHARD_TIMEOUT,
    // OOM cage), which is exactly how the 2026-08-24 pile accumulated.
    detail::reapStaleWorkDirs();

    std::lock_guard<std::mutex> lk(state.m);
    auto it = state.dirs.find(tag);
    if (it == state.dirs.end()) {
        fs::path dir = fs::path(detail::scratchBase()) / unique_stem(tag);
        std::error_code ec;
        fs::create_directories(dir, ec);
        it = state.dirs.emplace(tag, std::move(dir)).first;
    }
    return it->second;
}

}  // namespace bezel_test

// Test files call this unqualified, as they already do for unique_stem().
using bezel_test::wtScratchDir;
