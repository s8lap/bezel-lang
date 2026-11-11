// results_cache.hpp — the test harness's content-keyed RESULTS cache.
//
// WHY. The box is saturated by test EXECUTION, not compiles: the same tests run
// again and again on identical inputs (a producer runs its area on its tip, the
// reviewer runs it again on the same tip; many lanes run the same staging tip
// as a "base" arm). ccache shares compiles, scripts/shared_build.sh shares
// builds and the AOT bincache shares compiled programs, but RESULTS were never
// shared. This header lets tests/test_main.cpp record a PASS at the moment a
// test runs and serve it, instead of re-running the test, to a later run whose
// inputs are byte-identical.
//
// A CACHE THAT SERVES A STALE PASS IS WORSE THAN NO CACHE, so everything below
// fails closed:
//   * The cache is INERT unless the caller hands the process a key file
//     (BEZEL_RESULTS_CACHE_KEYS). Only scripts/agent_check.sh writes one
//     (through scripts/results_cache.sh). A binary run by hand, by
//     tests/run_parallel.sh, or by any gate script never sees one, and those
//     scripts also export BEZEL_RESULTS_CACHE=0, which turns it off here even
//     when a key file leaked into the environment. The gate is the acceptance
//     oracle and is never served from this cache.
//   * The key file maps a binary's IDENTITY (dev:ino:size:mtime_ns of the exact
//     file the shell hashed) to an artifact key the shell computed: the sha256
//     of this binary, of the bezelc / runtime / core archives it embeds the
//     paths of, the clean source tree it reads from, and the toolchain. This
//     process looks ITSELF up by stat()ing /proc/self/exe, so a key computed
//     for another (or an older) binary is never applied to this one.
//   * This process then mixes in every environment variable that can change a
//     test's behaviour (envAffectsKey: every BEZEL_* except a short list of
//     pure selection/plumbing knobs, plus the sanitizer, loader, compiler and
//     locale variables), so flipping e.g. BEZEL_AOT_DETECT_LEAKS is a miss.
//   * Only PASSES are stored, and only a pass that moved none of the harness's
//     loudness counters (sanitizer skip, sanitizer infra, checked-toolchain
//     skip, [ INFRA ], timeout, fork failure, [ LEAK ]). A FAIL, a crash or a
//     timeout (the process dies before it records anything) is never stored.
//   * Under BEZEL_AOT_FORCE_SANITIZE the cache is OFF: a sanitizer report
//     fired in a PASSING row is itself the ASan oracle's verdict (the gate's
//     san-scope attribution), and a served PASS cannot replay that report.
//   * Flake guard: ANY FAIL poisons the test for that key, whether or not a
//     PASS is already recorded: never served again, logged to
//     <dir>/poison.log. "Failed once, passed once" under one key is a flake,
//     and a later PASS must not turn it into a served PASS.
//   * Staging (BEZEL_RESULTS_CACHE_STAGE): agent_check.sh hands each run a
//     private stage directory. New passes are written THERE, not into the key
//     directory, and scripts/results_cache.sh `commit` promotes them only
//     after re-checking, AFTER the run, that the tree is still clean and every
//     artefact still hashes to the key; otherwise it drops them. A pass
//     computed while the tree went dirty is never stored under the clean key.
//
// Everything is per process and lock-free: entries are written to a unique
// temp name in the entry's own directory and rename()d into place, so many
// concurrent shards and lanes can write the same key safely, and a reader sees
// a whole entry or none. Entries carry an end marker, so a truncated file (a
// crash between write and rename on a non-journalled fs) reads as a miss.
//
// Included ONLY by tests/test_main.cpp and its own regression test
// (tests/support/results_cache_test.cpp), so editing it relinks the six test
// binaries and recompiles nothing else.
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ios>
#include <iterator>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

namespace bezel::test::results_cache {

// Bump when the entry format, the key derivation or the cacheability rules
// change: it is hashed into every key (and checked in the key file), so an old
// cache can never be read by a new harness.
inline constexpr const char* kSchema = "bezel.results-cache.v1";
inline constexpr const char* kDefaultDir = "/mnt/fast/test-results-cache";
// Captured output kept per entry (std::cout + std::cerr of the test body).
inline constexpr std::size_t kCaptureCap = 16u * 1024u;

inline std::string sha256Hex(std::string_view data) {
    const std::array<std::uint8_t, 32> d =
        llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t b : d) {
        out += hex[b >> 4];
        out += hex[b & 0xf];
    }
    return out;
}

// Does this environment variable take part in the key? Fail closed: EVERY
// BEZEL_* variable does unless it is on the short list of names that only
// select WHICH tests run, where output goes, or how the run is scheduled —
// none of which can change a single test's verdict. A new behaviour knob is
// therefore keyed the day it is added, with no edit here. (Enumerated
// 2026-11-12 by grepping tests/ and src/ for getenv; see the commit body.)
inline bool envAffectsKey(std::string_view name) {
    static constexpr std::string_view kVolatileBezel[] = {
        // selection / partition: which tests this process runs
        "BEZEL_TEST_FILTER", "BEZEL_TEST_LIST", "BEZEL_TEST_ENUMERATE",
        "BEZEL_SHARD_COUNT", "BEZEL_SHARD_INDEX",
        // where a summary is written
        "BEZEL_SHARD_SUMMARY",
        // scheduling: the machine-wide token, a held-token marker
        "BEZEL_JOBSERVER", "BEZEL_TEST_TOKEN_HELD",
        // the agent_check wrapper's own knobs (never read by a test)
        "BEZEL_GTEST_TIMEOUT", "BEZEL_AGENT_SCRATCH",
        "BEZEL_SHARED_BUILD_OFFER",
    };
    if (name.rfind("BEZEL_", 0) == 0) {
        if (name.rfind("BEZEL_RESULTS_CACHE", 0) == 0) return false;
        for (std::string_view v : kVolatileBezel)
            if (name == v) return false;
        return true;
    }
    static constexpr std::string_view kPrefixes[] = {
        "ASAN_", "LSAN_", "UBSAN_", "TSAN_", "MSAN_", "LD_", "LLVM_",
        "CCACHE_", "LC_",
        // clang driver rewriting (CCC_OVERRIDE_OPTIONS, ...), gcc-style
        // driver search paths, and glibc malloc tuning (MALLOC_ARENA_MAX,
        // MALLOC_*_THRESHOLD_, MALLOC_CHECK_, MALLOC_PERTURB_, ...)
        "CCC_", "GCC_", "MALLOC_",
    };
    for (std::string_view p : kPrefixes)
        if (name.rfind(p, 0) == 0) return true;
    static constexpr std::string_view kExact[] = {
        "PATH", "HOME", "LANG", "TZ", "CC", "CXX", "CFLAGS", "CXXFLAGS",
        "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "COMPILER_PATH",
        "SDKROOT", "GLIBC_TUNABLES",
    };
    for (std::string_view e : kExact)
        if (name == e) return true;
    return false;
}

// The keyed slice of an environment, sorted, one NAME=VALUE per line.
inline std::string envFingerprint(char** envp) {
    std::vector<std::string> kept;
    for (char** e = envp; e && *e; ++e) {
        const std::string_view kv(*e);
        const std::size_t eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        if (envAffectsKey(kv.substr(0, eq))) kept.emplace_back(kv);
    }
    std::sort(kept.begin(), kept.end());
    std::string out;
    for (const std::string& s : kept) {
        out += s;
        out += '\n';
    }
    return out;
}

// dev:ino:size:mtime_sec.mtime_nsec — the same string scripts/results_cache.sh
// builds with `stat -L -c '%d:%i:%s:%.9Y'`.
inline std::string statId(const char* path) {
    struct stat st {};
    if (::stat(path, &st) != 0) return {};
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%llu:%llu:%lld:%lld.%09ld",
                  static_cast<unsigned long long>(st.st_dev),
                  static_cast<unsigned long long>(st.st_ino),
                  static_cast<long long>(st.st_size),
                  static_cast<long long>(st.st_mtim.tv_sec),
                  static_cast<long>(st.st_mtim.tv_nsec));
    return buf;
}

// Write `content` to `path` atomically: a unique temp in the SAME directory,
// then rename(). No fsync — an entry is an optimisation, and the end marker
// turns a torn entry into a miss. Returns false on any failure (no file left).
inline bool atomicWrite(const std::string& path, const std::string& content) {
    static unsigned long long ctr = 0;
    const std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." +
                            std::to_string(++ctr);
    const int fd = ::open(tmp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::size_t off = 0;
    bool ok = true;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tmp.c_str(), path.c_str()) == 0) return true;
    ::unlink(tmp.c_str());
    return false;
}

inline bool mkdirP(const std::string& dir) {
    if (dir.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos != std::string::npos) {
        pos = dir.find('/', pos + 1);
        cur = dir.substr(0, pos);
        if (cur.empty()) continue;
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool fileExists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

inline std::string readFile(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

enum class Mode {
    Off,        // neither read nor write
    ReadWrite,  // serve recorded passes; record new passes; poison on contradiction
    Verify,     // never serve; record passes; poison on contradiction
};

// BEZEL_RESULTS_CACHE: unset/""/1/on/rw -> ReadWrite, verify -> Verify,
// 0/off/no/false -> Off. Any other value is Off (fail closed) with a reason.
inline Mode parseMode(const char* v, std::string& why) {
    const std::string s = v ? v : "";
    if (s.empty() || s == "1" || s == "on" || s == "rw" || s == "yes" ||
        s == "true")
        return Mode::ReadWrite;
    if (s == "verify") return Mode::Verify;
    if (s == "0" || s == "off" || s == "no" || s == "false") {
        why = "BEZEL_RESULTS_CACHE=" + s;
        return Mode::Off;
    }
    why = "unrecognised BEZEL_RESULTS_CACHE='" + s + "' (fail closed)";
    return Mode::Off;
}

// Tees a std::streambuf: every byte goes to the original buffer unchanged (so
// the stream's own buffering and ordering are exactly as before) and the first
// kCaptureCap bytes are also kept for the cache entry.
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* orig, std::string& cap) : orig_(orig), cap_(cap) {}

protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof()))
            return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        keep(&ch, 1);
        return orig_->sputc(ch);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        keep(s, static_cast<std::size_t>(n));
        return orig_->sputn(s, n);
    }
    int sync() override { return orig_->pubsync(); }

private:
    void keep(const char* s, std::size_t n) {
        if (cap_.size() >= kCaptureCap) return;
        cap_.append(s, std::min(n, kCaptureCap - cap_.size()));
    }
    std::streambuf* orig_;
    std::string& cap_;
};

// The snapshot of every loudness counter a PASS must not have moved to be
// recorded. Filled by the caller (test_main.cpp knows the counters).
using Counters = std::array<std::uint64_t, 8>;

class Cache {
public:
    // `keysEnv` = BEZEL_RESULTS_CACHE_KEYS, `modeEnv` = BEZEL_RESULTS_CACHE,
    // `dirEnv` = BEZEL_RESULTS_CACHE_DIR, `exe` = this binary (/proc/self/exe).
    void configure(const char* keysEnv, const char* modeEnv, const char* dirEnv,
                   const char* exe, char** envp, bool forceSanitize) {
        requested_ = keysEnv && *keysEnv;
        if (!requested_) return;  // inert: prints nothing, byte-identical run
        std::string why;
        mode_ = parseMode(modeEnv, why);
        if (mode_ == Mode::Off) return off(why);
        if (forceSanitize)
            return off("BEZEL_AOT_FORCE_SANITIZE: a fired report in a passing "
                       "row is the ASan oracle's verdict and a cached PASS "
                       "cannot replay it");
        const std::string self = statId(exe);
        if (self.empty()) return off("cannot stat this binary");
        std::ifstream in(keysEnv);
        if (!in) return off("key file unreadable");
        std::string line, artifact;
        while (std::getline(in, line)) {
            // <schema> TAB <stat id> TAB <artifact key> [TAB <path>]
            const std::size_t t1 = line.find('\t');
            const std::size_t t2 =
                t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            if (line.compare(0, t1, kSchema) != 0) continue;
            if (line.compare(t1 + 1, t2 - t1 - 1, self) != 0) continue;
            const std::size_t t3 = line.find('\t', t2 + 1);
            artifact = line.substr(t2 + 1, t3 == std::string::npos
                                               ? std::string::npos
                                               : t3 - t2 - 1);
            break;
        }
        if (artifact.size() != 64)
            return off("this binary is not in the key file (rebuilt since the "
                       "key was computed?)");
        env_ = envFingerprint(envp);
        key_ = sha256Hex(std::string(kSchema) + "\n" + artifact + "\n" + env_);
        root_ = (dirEnv && *dirEnv) ? dirEnv : kDefaultDir;
        keyDir_ = root_ + "/v1/" + key_;
        if (!mkdirP(keyDir_)) return off("cannot create " + keyDir_);
        if (const char* st = std::getenv("BEZEL_RESULTS_CACHE_STAGE");
            st && *st) {
            stageDir_ = std::string(st) + "/" + key_;
            if (!mkdirP(stageDir_)) return off("cannot create " + stageDir_);
        }
        const std::string desc = keyDir_ + "/KEY";
        if (!fileExists(desc))
            atomicWrite(desc, std::string(kSchema) + "\nartifact=" + artifact +
                                  "\n--- env ---\n" + env_);
    }

    bool requested() const { return requested_; }
    bool active() const { return requested_ && mode_ != Mode::Off; }
    Mode mode() const { return mode_; }
    const std::string& key() const { return key_; }
    const std::string& keyDir() const { return keyDir_; }

    std::string entryPath(const std::string& full, const char* ext) const {
        return keyDir_ + "/" + sha256Hex(full).substr(0, 40) + ext;
    }

    // A recorded, unpoisoned PASS for `full` under this key? Sets `info` to a
    // one-line description of the entry for the [ CACHED ] line.
    bool tryServe(const std::string& full, std::string& info) {
        if (mode_ != Mode::ReadWrite) return false;
        if (fileExists(entryPath(full, ".poison"))) {
            ++poisonedSkips_;
            return false;
        }
        const std::string body = readFile(entryPath(full, ".pass"));
        if (!validPass(body, full)) return false;
        std::string dur = field(body, "duration_ms");
        std::string when = field(body, "recorded_utc");
        info = "pass recorded " + when + " in " + dur + " ms";
        ++reused_;
        // Prune keys on the key directory's mtime: a served key is in use.
        if (!touchedKeyDir_)
            touchedKeyDir_ = ::utimensat(AT_FDCWD, keyDir_.c_str(), nullptr, 0) == 0;
        return true;
    }

    // Record the outcome of an EXECUTED test.
    // `extra` is zero or more "name=value\n" lines stored verbatim (the
    // harness puts the test's AOT bincache counters there; informational).
    void record(const std::string& full, bool failed, bool clean,
                long long durationMs, const std::string& captured,
                const std::string& extra = {}) {
        if (!requested_) return;
        ++executed_;
        if (!active()) return;
        const std::string passPath = entryPath(full, ".pass");
        const std::string poisonPath = entryPath(full, ".poison");
        if (failed) {
            // Flake guard: a FAIL poisons the test under this key, recorded
            // PASS or not, so a later PASS is never stored or served.
            if (!fileExists(poisonPath))
                poison(full, poisonPath, durationMs,
                       validPass(readFile(passPath), full));
            return;
        }
        if (!clean) {
            ++uncacheable_;
            return;
        }
        if (fileExists(poisonPath)) return;  // poisoned stays poisoned
        std::string e = std::string(kSchema) + "\n";
        e += "name=" + full + "\n";
        e += "key=" + key_ + "\n";
        e += "status=pass\n";
        e += "duration_ms=" + std::to_string(durationMs) + "\n";
        e += "recorded_utc=" + utcNow() + "\n";
        e += "pid=" + std::to_string(::getpid()) + "\n";
        e += "output_bytes=" + std::to_string(captured.size()) + "\n";
        e += extra;
        e += "--- output ---\n";
        e += captured;
        if (!captured.empty() && captured.back() != '\n') e += '\n';
        e += "--- end ---\n";
        // Staged when agent_check gave a stage: promoted only by
        // `results_cache.sh commit` after the run re-checks the tree.
        const std::string target =
            stageDir_.empty()
                ? passPath
                : stageDir_ + "/" + sha256Hex(full).substr(0, 40) + ".pass";
        if (atomicWrite(target, e)) ++stored_;
    }

    // RESULTS CACHE: R reused, E executed, S stored, P poisoned, U unstored,
    // Q poisoned-reruns (key K, mode M) — or "(off: why)". Six counts in fixed
    // positions, so scripts/agent_check.sh can sum them across shards. Printed
    // only when a key file was given, so a run without one (the gate, a hand
    // run) is byte-identical to before.
    std::string summaryLine() const {
        std::string s = "RESULTS CACHE: " + std::to_string(reused_) +
                        " reused, " + std::to_string(executed_) +
                        " executed, " + std::to_string(stored_) + " stored, " +
                        std::to_string(poisoned_) + " poisoned, " +
                        std::to_string(uncacheable_) + " unstored, " +
                        std::to_string(poisonedSkips_) + " poisoned-reruns";
        if (!active()) return s + " (off: " + offReason_ + ")\n";
        return s + " (key " + key_.substr(0, 12) + ", mode " +
               (mode_ == Mode::Verify ? "verify" : "rw") + ")\n";
    }

private:
    void off(const std::string& why) {
        mode_ = Mode::Off;
        offReason_ = why;
    }

    static std::string field(const std::string& body, const char* name) {
        const std::string pfx = std::string("\n") + name + "=";
        const std::size_t p = body.find(pfx);
        if (p == std::string::npos) return {};
        const std::size_t b = p + pfx.size();
        return body.substr(b, body.find('\n', b) - b);
    }

    bool validPass(const std::string& body, const std::string& full) const {
        static const std::string kEnd = "--- end ---\n";
        if (body.compare(0, std::strlen(kSchema), kSchema) != 0) return false;
        if (body.size() < kEnd.size() ||
            body.compare(body.size() - kEnd.size(), kEnd.size(), kEnd) != 0)
            return false;
        return field(body, "name") == full && field(body, "key") == key_ &&
               field(body, "status") == "pass";
    }

    void poison(const std::string& full, const std::string& poisonPath,
                long long durationMs, bool contradicted) {
        const std::string line = utcNow() + "\tPOISON\t" + key_ + "\t" + full +
                                 "\tpid=" + std::to_string(::getpid()) +
                                 "\tfail_ms=" + std::to_string(durationMs) +
                                 "\n";
        atomicWrite(poisonPath, std::string(kSchema) + "\n" + line);
        // One write(2) with O_APPEND, under PIPE_BUF: concurrent lines never
        // interleave.
        const int fd = ::open((root_ + "/poison.log").c_str(),
                              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) {
            (void)!::write(fd, line.data(), line.size());
            ::close(fd);
        }
        std::fprintf(stderr,
                     "[ POISON ] %s: %s; the results cache will never store or "
                     "serve a PASS for it under key %s (logged to "
                     "%s/poison.log)\n",
                     full.c_str(),
                     contradicted
                         ? "a recorded PASS was contradicted by this FAIL"
                         : "this FAIL poisons the test",
                     key_.substr(0, 12).c_str(), root_.c_str());
        ++poisoned_;
    }

    static std::string utcNow() {
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        ::gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    bool requested_ = false;
    Mode mode_ = Mode::Off;
    std::string offReason_;
    std::string env_, key_, root_, keyDir_;
    std::string stageDir_;
    bool touchedKeyDir_ = false;
    std::size_t reused_ = 0, executed_ = 0, stored_ = 0, poisoned_ = 0,
                uncacheable_ = 0, poisonedSkips_ = 0;
};

}  // namespace bezel::test::results_cache
