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

namespace bezel::native_test {

namespace fs = std::filesystem;

// Absolute path to repo-root/src/runtime, derived from this header's __FILE__
// (tests/native_test_harness.hpp → repo root).
inline fs::path runtimeSourceDir() {
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
// The master lives under /var/tmp/bezel-scratch (DISK-backed — never
// fs::temp_directory_path(): per-shard TMPDIRs are tmpfs, and 16 shards each
// rebuilding ~100MB of ASan objects into RAM both thrashes the box and costs
// minutes per shard), keyed by a hash of the source dir so parallel worktrees
// get distinct masters. Cross-process safety + dedup via flock on a sibling
// lock file: the first process builds, the rest wait then reuse. Freshness is
// mtime-based against every file in src/runtime (sources AND headers), so an
// edited runtime invalidates the shared master without a manual clean.
inline const std::string& sanitizerRuntimeMaster(const std::string& san) {
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
    fs::path dir = fs::path("/var/tmp/bezel-scratch/rt-objs") /
                   std::to_string(std::hash<std::string>{}(srcDir.string()));
    fs::create_directories(dir, ec);
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
