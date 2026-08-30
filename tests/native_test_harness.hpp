#pragma once

// Shared helpers for the "native ASan/TSan" test leg used across ~80 test files.
//
// #1414 stage 2 split src/runtime/bezel_runtime.cpp into N per-domain TUs
// (src/runtime/bezel_runtime_*.cpp). The native leg still needs ONE
// sanitizer-instrumented runtime object to link against, so asanRuntimeObject()
// now compiles every TU under -fsanitize=address and `ld -r`-combines the
// objects into a single relocatable .o — preserving the single-path API:
//
//   #include "native_test_harness.hpp"
//   const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
//   if (rtObj.empty()) { cleanup(); return -1; }   // toolchain/build unavailable → SKIP
//
// The N-source compile + ld -r happens at most once per sanitizer (memoized).
// buildRuntimeSanitizerObject(san, destObj) is the general form used by the few
// tests that build the runtime under an explicit sanitizer (e.g. TSan) or to a
// lane-local path: it ensures the combined object exists and copies it to
// destObj.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "support/scratch_dir.hpp"  // the ONE scratch-root policy

namespace bezel::native_test {

namespace fs = std::filesystem;

// fix #3626: a SINGLE counting chokepoint for "the native sanitizer runtime
// was unavailable". Every one of the ~122 test files that need the combined
// ASan/TSan runtime object funnels through sanitizerRuntimeMaster() below
// (via asanRuntimeObject() or buildRuntimeSanitizerObject()) — none of those
// 122 files are touched by this fix. Counting here is what makes "reported,
// not silent" a property of the harness, rather than a convention each of
// 122 files' own `if (rtObj.empty()) return -1;` has to individually honour
// (most do; the ones that don't are exactly how #3626 was found — an
// unavailable oracle reporting a clean PASS).
//
// Incremented once per CALL that observes an empty result — including cache
// hits (sanitizerRuntimeMaster's build attempt is memoized to at most once
// per sanitizer per process; the calls after that just read the cached ""),
// so the count tracks how many test-observable checks were affected by the
// outage, not merely whether the underlying build was attempted.
// tests/test_main.cpp reads this once per process and prints it loudly in
// the per-binary summary (and the structured JSON shard summary) regardless
// of whether it is zero — "0" is then a visible confirmation the oracle ran,
// not the absence of a signal. tests/run_parallel.sh sums it across every
// shard's JSON and fails the sharded run (the mechanism scripts/gate.sh's
// acceptance verdict depends on) when the total is nonzero: an acceptance
// gate must not pass with a verification leg silently switched off, the
// same shape as the #3002 coverage floor.
inline std::atomic<std::size_t>& nativeSanitizerUnavailableSkips() {
    static std::atomic<std::size_t> n{0};
    return n;
}

// Absolute path to repo-root/src/runtime.
//
// It MUST be absolute: it is the input to sanitizerRuntimeMaster(), whose whole
// output (the ASan-instrumented runtime object ~123 test files link against) is
// "" — i.e. SKIP — when the directory does not resolve. A relative answer makes
// that outcome depend on the CWD of whatever runs the test binary, and the two
// callers disagree: run_parallel.sh inherits its caller's cwd, and gate.sh runs
// from the WORKTREE ROOT (`cd $(git rev-parse --show-toplevel)`, gate.sh:71).
//
// Primary channel: BEZEL_RUNTIME_SRC_DIR, an absolute -D from
// tests/CMakeLists.txt. A -D value is not a path argument, so a path-REWRITING
// compiler wrapper leaves it alone.
//
// The ⚠ fallback is the ORIGINAL derivation, from this header's own __FILE__,
// and it is the reason the primary channel exists: __FILE__ is absolute only
// when the compiler was invoked with an absolute -I. ccache's `base_dir`
// rewrites every absolute path under it to one relative to the compiler's cwd
// AND PASSES THE REWRITTEN ARGV TO THE REAL COMPILER, so with
// `base_dir = /home/stepan` this expanded to
// `../../../../../home/.../tests/native_test_harness.hpp` — resolvable from the
// CMake build dir and nowhere else. Measured 2026-08-30: identical binary,
// identical tree; run from <build>/tests the ASan rows of
// ownedarray_forin_using_var_1287 / owning_array_drain_getowningenum_2218 /
// owning_enumerator_pending_value_1794 / channel_recv_all_1724 are 4/5/3/8
// PASS (20 rows), and run from the worktree root they are 3/2/0/7 — 8 rows
// FAIL. The fallback is kept so a TU compiled outside CMake still works, but it
// is now second, and it is pinned by
// tests/support/runtime_source_dir_absolute_test.cpp.
inline fs::path runtimeSourceDir() {
#ifdef BEZEL_RUNTIME_SRC_DIR
    {
        std::error_code ec;
        fs::path fromBuild(BEZEL_RUNTIME_SRC_DIR);
        if (fromBuild.is_absolute() && fs::is_directory(fromBuild, ec) && !ec)
            return fromBuild.lexically_normal();
    }
#endif
    fs::path here(__FILE__);              // tests/native_test_harness.hpp
    return here.parent_path()             // tests/
               .parent_path()             // repo root
           / "src" / "runtime";
}

// The per-domain runtime translation units (src/runtime/bezel_runtime_*.cpp),
// sorted for a stable order. Post-#1414 there is no single bezel_runtime.cpp.
inline std::vector<std::string> runtimeSourceFiles() {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(runtimeSourceDir(), ec)) {
        const fs::path& p = e.path();
        if (p.extension() == ".cpp" &&
            p.filename().string().rfind("bezel_runtime_", 0) == 0)
            v.push_back(p.string());
    }
    std::sort(v.begin(), v.end());
    return v;
}

// Deprecated (#1414): the runtime is no longer a single .cpp. Retained for
// source compatibility; returns the src/runtime directory. New code should use
// runtimeSourceFiles() / asanRuntimeObject() / buildRuntimeSanitizerObject().
inline std::string runtimeSourcePath() { return runtimeSourceDir().string(); }

// Build (once per sanitizer per SOURCE TREE, shared across processes) a single
// relocatable object that contains every runtime TU compiled under
// -fsanitize=<san>, `ld -r`-combined. Returns the cached master object path,
// or "" if the toolchain is unavailable or any step fails (callers then SKIP).
//
// The master lives beside the shared scratch base (DISK-backed — never
// fs::temp_directory_path(): that is tmpfs, and 16 shards each rebuilding
// ~100MB of ASan objects into RAM both thrashes the box and costs minutes per
// shard), keyed by a hash of the source dir so parallel worktrees
// get distinct masters. Cross-process safety + dedup via flock on a sibling
// lock file: the first process builds, the rest wait then reuse. Freshness is
// mtime-based against every file in src/runtime (sources AND headers), so an
// edited runtime invalidates the shared master without a manual clean.
// Age-gated eviction for the rt-objs cache. The key is a hash of the runtime
// SOURCE DIR, i.e. of the worktree path — so every lane worktree ever created
// mints a fresh key that is never reused once that worktree is reaped. Without
// eviction the cache only grows: measured 2026-08-24 at 717 keys / 8.7 GB, of
// which 563 belonged to worktrees that no longer existed. Entries are pure
// build products (a `ld -r` of the runtime under one sanitizer), so dropping a
// live one costs a rebuild, never correctness.
//
// Runs once per process, and only removes keys untouched for
// BEZEL_RT_OBJS_MAX_AGE_DAYS (default 14) — far longer than any single run, so
// it can never race a concurrent shard's in-flight build.
inline void evictStaleRtObjs(const std::filesystem::path& rtRoot) {
    namespace fs = std::filesystem;
    static const bool once = [&] {
        const char* e = std::getenv("BEZEL_RT_OBJS_MAX_AGE_DAYS");
        const int days = (e && *e) ? std::atoi(e) : 14;
        if (days <= 0) return true;  // 0 disables
        const auto maxAge = std::chrono::hours(24 * days);
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        for (fs::directory_iterator it(rtRoot, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2) || e2) continue;
            const auto mt = fs::last_write_time(it->path(), e2);
            if (e2) continue;
            if (now - mt > maxAge) fs::remove_all(it->path(), e2);
        }
        return true;
    }();
    (void)once;
}

// Renamed from sanitizerRuntimeMaster (fix #3626): the original body is
// unchanged below, wrapped by the thin counted forwarder that follows it —
// callers keep calling sanitizerRuntimeMaster(); this split is what lets the
// counting live in ONE place without duplicating the build/memoize logic.
inline const std::string& sanitizerRuntimeMasterUncounted(
    const std::string& san) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(san);
    if (it != cache.end()) return it->second;
    std::string& out = cache[san];  // default "" — memoize failures too so we
                                    // don't re-attempt a broken toolchain build.

    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return out;
    std::vector<std::string> srcs = runtimeSourceFiles();
    if (srcs.empty()) return out;

    std::error_code ec;
    const fs::path srcDir = runtimeSourceDir();
    // Derived from the ONE scratch-root policy rather than hardcoded, so this
    // cache can never be pointed at /tmp by accident (support/scratch_dir.hpp).
    const fs::path rtRoot =
        fs::path(bezel::test::aot_detail::scratchBase()).parent_path() /
        "rt-objs";
    fs::path dir = rtRoot /
                   std::to_string(std::hash<std::string>{}(srcDir.string()));
    fs::create_directories(dir, ec);
    evictStaleRtObjs(rtRoot);
    if (ec) return out;

    const std::string combined =
        (dir / ("bezel_rt_" + san + "_combined.o")).string();

    // Newest mtime across src/runtime (TUs + bezel_runtime*.h/.hpp).
    fs::file_time_type newestSrc = fs::file_time_type::min();
    for (const auto& e : fs::directory_iterator(srcDir, ec)) {
        auto t = fs::last_write_time(e.path(), ec);
        if (!ec && t > newestSrc) newestSrc = t;
    }
    auto isFresh = [&]() {
        std::error_code fec;
        auto t = fs::last_write_time(combined, fec);
        return !fec && t >= newestSrc;
    };

    int lockFd = ::open((dir / ("build_" + san + ".lock")).c_str(),
                        O_CREAT | O_RDWR, 0666);
    if (lockFd < 0) return out;
    ::flock(lockFd, LOCK_EX);  // serialize check+build across processes
    if (isFresh()) {
        ::flock(lockFd, LOCK_UN); ::close(lockFd);
        out = combined;
        return out;
    }
    const std::string inc = "-I" + srcDir.string();
    std::string objs;
    bool ok = true;
    for (const std::string& s : srcs) {
        std::string stem = fs::path(s).stem().string();
        std::string obj = (dir / ("bezel_rt_" + san + "_" + stem + ".o")).string();
        std::string cmd = "clang++ -std=c++20 -g -fsanitize=" + san +
                          " -pthread " + inc + " -c " + s + " -o " + obj +
                          " > /dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) { ok = false; break; }
        objs += " " + obj;
    }
    if (ok) {
        // ld -r to a temp name + rename: a waiter that sees `combined` sees a
        // complete object, never a partial write.
        std::string tmp = combined + ".tmp";
        std::string ld = "ld -r -o " + tmp + objs + " > /dev/null 2>&1";
        ok = std::system(ld.c_str()) == 0;
        if (ok) { fs::rename(tmp, combined, ec); ok = !ec; }
    }
    ::flock(lockFd, LOCK_UN); ::close(lockFd);
    if (ok) out = combined;
    return out;
}

// fix #3626: the counted forwarder every caller (asanRuntimeObject(),
// buildRuntimeSanitizerObject()) actually calls. A one-line wrapper around
// sanitizerRuntimeMasterUncounted() rather than instrumenting that function's
// several early-return points individually — it has ~6 of them, and a new one
// added later would silently bypass counting if the increment lived inline.
// Wrapping the call instead means every exit path, present or future, is
// covered by construction.
inline const std::string& sanitizerRuntimeMaster(const std::string& san) {
    const std::string& result = sanitizerRuntimeMasterUncounted(san);
    if (result.empty())
        nativeSanitizerUnavailableSkips().fetch_add(1, std::memory_order_relaxed);
    return result;
}

// Ensure a <san>-instrumented combined runtime object exists at destObj (copied
// from the memoized master). Returns false on toolchain/build failure (SKIP).
// Replaces the pre-#1414 single-file `clang++ -c bezel_runtime.cpp -o destObj`.
inline bool buildRuntimeSanitizerObject(const std::string& san,
                                        const std::string& destObj) {
    const std::string& master = sanitizerRuntimeMaster(san);
    if (master.empty()) return false;
    std::error_code ec;
    fs::path dp(destObj);
    if (dp.has_parent_path()) fs::create_directories(dp.parent_path(), ec);
    fs::copy_file(master, destObj, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// The ASan-instrumented combined runtime object — the common native leg. A
// single stable path; ~80 test files link it. "" if the toolchain is
// unavailable or the build fails, so callers SKIP.
inline const std::string& asanRuntimeObject() {
    return sanitizerRuntimeMaster("address");
}

} // namespace bezel::native_test
