#pragma once

// aot_link_mode.hpp — THE definition of the self-contained (musl) link knob for
// the AOT harness (#6706).
//
// The release bezelc links executables through the driver's self-contained
// mode (`--link=self-contained`, Driver::LinkMode::SelfContained, #6654): a
// static ld.lld against a staged musl + libc++ + runtime bundle. The suite
// normally links through the host clang against glibc. This knob runs the
// suite's programs the way a release links them:
//
//   BEZEL_AOT_LINK=self-contained  BEZEL_AOT_BUNDLE_DIR=<dir>
//
// where <dir> is a staged `lib/bezel/x86_64-linux-musl` (the layout
// Driver::selfContainedLinkArgs reads). Unset, empty, or `system` is the
// default host link, and then NOTHING here changes behaviour: no link command,
// no cache key, no output line moves (aot_link_mode_6706_test pins that).
//
// Any other value, or the self-contained mode with no usable bundle, aborts the
// test binary with a message. A misspelt knob must not quietly test glibc.
//
// What stays on the system link under the knob, each COUNTED, never silent:
//   * sanitized builds (`runAotChecked*`, `BEZEL_AOT_FORCE_SANITIZE`): ASan has
//     no musl runtime and the driver rejects --sanitize=address with the
//     self-contained link;
//   * programs inside a BEZEL_SELF_CONTAINED_DIVERGES scope: an explicit,
//     listed, MEASURED divergence, with an issue number and a reason.
// test_main.cpp prints `SELF-CONTAINED LINK: …` with the three counts whenever
// the knob is on.
//
// DEPENDENCY-FREE (<atomic>, <cstdio>, <cstdlib>, <string>) for the same reason
// as force_sanitize.hpp: test_main.cpp includes it.

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bezel::test {

namespace link_mode_detail {

[[noreturn]] inline void fatal(const std::string& why) {
    std::fprintf(stderr, "bezel_tests: %s\n", why.c_str());
    std::fflush(stderr);
    std::abort();
}

}  // namespace link_mode_detail

// True when BEZEL_AOT_LINK=self-contained. Memoized (the environment does not
// change under a running test binary).
inline bool aotLinkSelfContained() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_LINK");
        if (!e || *e == '\0') return false;
        const std::string s(e);
        if (s == "system") return false;
        if (s == "self-contained") return true;
        link_mode_detail::fatal(
            "BEZEL_AOT_LINK='" + s +
            "' is not a link mode (expected self-contained or system; #6706)");
    }();
    return v;
}

// The bundle directory BEZEL_AOT_BUNDLE_DIR names. Only meaningful when
// aotLinkSelfContained(); aot_support.hpp validates it (bin/ld.lld present)
// and resolves it to an absolute path before the first link.
inline const std::string& aotBundleDirEnv() {
    static const std::string* v = new std::string([] {
        const char* e = std::getenv("BEZEL_AOT_BUNDLE_DIR");
        return std::string(e ? e : "");
    }());
    return *v;
}

// Builds that went through the self-contained link (counted at the harness's
// build entry, cache hits included: a hit serves a product of the same mode).
inline std::atomic<std::size_t>& selfContainedLinkRuns() {
    static std::atomic<std::size_t>* v = new std::atomic<std::size_t>(0);
    return *v;
}

// Sanitized builds kept on the system link under the knob.
inline std::atomic<std::size_t>& selfContainedSanitizedOnSystem() {
    static std::atomic<std::size_t>* v = new std::atomic<std::size_t>(0);
    return *v;
}

// Builds kept on the system link by a BEZEL_SELF_CONTAINED_DIVERGES marker.
inline std::atomic<std::size_t>& selfContainedDivergentOnSystem() {
    static std::atomic<std::size_t>* v = new std::atomic<std::size_t>(0);
    return *v;
}

// Nesting depth of active divergence markers on this thread.
inline int& selfContainedDivergenceDepth() {
    thread_local int depth = 0;
    return depth;
}

// The marker. While one is alive, the AOT builds on this thread link through
// the system toolchain even under the knob. Add one ONLY for a test MEASURED to
// diverge under the self-contained link for a reason that is a libc difference
// (glibc-specific output, say), never speculatively, and always with the
// issue that records it. tests/README.md lists every marker; `git grep
// BEZEL_SELF_CONTAINED_DIVERGES tests/` is the list.
class SelfContainedDivergence {
public:
    SelfContainedDivergence(int issue, const char* reason) {
        ++selfContainedDivergenceDepth();
        if (aotLinkSelfContained())
            std::printf("[ SELF-CONTAINED DIVERGENCE ] #%d: %s (kept on the "
                        "system link)\n",
                        issue, reason);
    }
    ~SelfContainedDivergence() { --selfContainedDivergenceDepth(); }
    SelfContainedDivergence(const SelfContainedDivergence&) = delete;
    SelfContainedDivergence& operator=(const SelfContainedDivergence&) = delete;
};

#define BEZEL_SC_CAT2_(a, b) a##b
#define BEZEL_SC_CAT_(a, b) BEZEL_SC_CAT2_(a, b)
#define BEZEL_SELF_CONTAINED_DIVERGES(issue, reason)                       \
    ::bezel::test::SelfContainedDivergence BEZEL_SC_CAT_(bezelScDiverge_, \
                                                         __LINE__) {      \
        (issue), (reason)                                                 \
    }

}  // namespace bezel::test
