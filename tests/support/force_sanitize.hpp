#pragma once

// force_sanitize.hpp — THE definition of the whole-suite ASan sweep switch.
//
// `scripts/gate.sh --full` puts the whole suite under generated-code
// AddressSanitizer by exporting `BEZEL_AOT_FORCE_SANITIZE=address` (#1822).
// Three places need to know that: `runAot` (support/aot_support.hpp), the
// hand-rolled `Driver::compileFile` runners (support/driver_run_support.hpp),
// and the shard-summary writer that records which oracle a shard ran under
// (test_main.cpp). They had three independent copies of the parse, which is a
// drift channel with no compiler check on it — and the drift is not theoretical:
// the runners had no copy at all, which is precisely the hole
// driver_run_support.hpp exists to close.
//
// This header is deliberately DEPENDENCY-FREE (<cstdlib> + <string>) so
// test_main.cpp — which otherwise pulls in nothing from the compiler — can
// include it without dragging in Driver.hpp and LLVM.

#include <cstdlib>
#include <string>

namespace bezel::test {

// True when the whole-suite sweep is on: BEZEL_AOT_FORCE_SANITIZE set to
// "address" / "asan" / "1". Unset/empty (the default) leaves normal runs
// byte-for-byte unchanged. Memoized once per process — the environment does not
// change under a running test binary, and the memo keeps this callable from a
// hot path.
inline bool forceSanitizeAddress() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORCE_SANITIZE");
        if (!e || *e == '\0') return false;
        std::string s(e);
        return s == "address" || s == "asan" || s == "1";
    }();
    return v;
}

// §22.16 / issue #4386: the suite-wide checked-contracts override, parallel
// to `forceSanitizeAddress` above. When `BEZEL_AOT_FORCE_CHECKED_CONTRACTS`
// is set to `1` / `on` / `true`, every `runAot` call sets
// `opts.checkedContracts = true` so the suite runs under checked-contracts
// mode uniformly — used by the gate's "force the whole suite under
// checked-contracts" sweep, mirroring the ASan sweep above. Unset/empty (the
// default) leaves `runAot`'s own test default in effect.
inline bool forceCheckedContracts() {
    static const bool v = [] {
        const char* e = std::getenv("BEZEL_AOT_FORCE_CHECKED_CONTRACTS");
        if (!e || *e == '\0') return false;
        std::string s(e);
        return s == "1" || s == "on" || s == "true";
    }();
    return v;
}

}  // namespace bezel::test
