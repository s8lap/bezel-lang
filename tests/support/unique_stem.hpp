// Per-process unique temp-file stem. The old std::to_string(std::rand())-based
// naming was the root cause of the #3002-class xmod COVERAGE SHORT: std::rand()
// is a single global PRNG shared across every test in the same shard (it is
// not thread-safe and the seed is per-process), and the birthday-paradox
// collision probability on ~186 tests per shard is ~0.8% — a collision
// overwrites the loser's source file mid-shard, garbles the in-flight
// compileObject child's read, and the shard crashes. The fix: a process-wide
// atomic counter combined with getpid() for cross-process uniqueness. O(1), no
// syscall per call beyond the once-cached getpid(), and the stem is unique
// across the whole /tmp scratch dir.
//
// Usage: replace every occurrence of
//     std::string stem = "<prefix>" + std::to_string(std::rand());
// with
//     std::string stem = unique_stem("<prefix>");
// and `#include "support/unique_stem.hpp"` once per TU.

#pragma once

#include <atomic>
#include <string>
#include <unistd.h>

namespace bezel_test {

inline std::atomic<uint64_t> &g_temp_stem_counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline std::string unique_stem(const char *prefix) {
    return std::string(prefix) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(
               g_temp_stem_counter().fetch_add(1, std::memory_order_relaxed));
}

}  // namespace bezel_test

// Test files commonly call this from anonymous namespaces without qualifying;
// provide a using-declaration so the call site stays concise.
using bezel_test::unique_stem;