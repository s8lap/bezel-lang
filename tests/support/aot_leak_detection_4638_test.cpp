// fix #4638 — `runAot` can now LEAK-CHECK the generated program, opt-in, and
// the check is proved able to FAIL.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
//
// A generated program could leak heap memory and the whole suite stayed green,
// INCLUDING under the gate's `--full` ASan oracle, because
// `BEZEL_AOT_FORCE_SANITIZE=address` enables AddressSanitizer without enabling
// LeakSanitizer's exit-time check: `runAot` pinned
// `ASAN_OPTIONS=detect_leaks=0:abort_on_error=0` (#1822). Measured 2026-09-23
// on the #4619 membership-boxing program (500 boxed queries, 24 B apiece,
// leaked by design and with the reasons recorded):
//
//   runAotChecked(leak500)                   -> LSan FIRES, 12000 B / 500 obj
//   runAot(leak500, sanitizeAddress = true)  -> clean, exit 2      <-- the gap
//   runAot(leak500)                          -> clean, exit 2
//   bezelc --sanitize=address, bare run      -> LSan FIRES, 12000 B / 500 obj
//
// So the leak-checking MECHANISM was never broken — `runAotChecked`, the
// harness whose documented contract is dispose-exactly-once, does check and
// can fail. `runAot`, which never claimed to, does not. That is the whole
// defect, and it is narrower than "the harness does not leak-check".
//
// ── WHY THE FIX IS A KNOB AND NOT A FLIP ───────────────────────────────────
//
// `detect_leaks=0` in `runAot` is deliberate and still correct by default:
// §28.2 checked-disposal QUARANTINES disposed objects rather than freeing them
// (issues #621/#627 — "a deliberate memory leak in checked builds"); a negative
// test that exits via an uncaught exception legitimately leaves heap live; and
// LSan's `exitcode` supersedes the program's own, which is precisely what cost
// #4532 a gate cycle. The fix therefore ADDS a third oracle,
// `BEZEL_AOT_DETECT_LEAKS` (off | report | strict), rather than changing the
// default one. See tests/support/force_sanitize.hpp for the full rationale.
//
// ── WHAT IS PINNED HERE ────────────────────────────────────────────────────
//
//  1. OFF is blind — the pre-fix behaviour, byte for byte. This row is the
//     "green with detection off" half of the fail-without proof.
//  2. STRICT actually FAILS on the known-positive, and the diagnostic carries
//     the byte count. A leak check nobody has seen fail is the vacuous oracle
//     this issue is about, so this row is the point of the file.
//  3. STRICT stays green on a leak-free program of the SAME SHAPE, so row 2
//     cannot be satisfied by an oracle that always fires.
//  4. REPORT is VERDICT-PRESERVING: same `ran`, same `exitCode`, no error —
//     and the counter moved. This is the invariant that makes it safe to turn
//     the census on across the whole suite.
//  5. The leak-only/ASan-error DISTINCTION, unit-tested on report text, so
//     Report mode can never soften a use-after-free into a note.
//  6. Mode parsing, including FAIL-CLOSED on an unrecognised value — a typo
//     must not silently disarm the oracle, which is the failure shape of the
//     whole issue.
//
// §8.6 / §28.2, v0.307.

#include <atomic>
#include <cstdlib>
#include <string>

#include "test_harness.hpp"
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/force_sanitize.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::LeakDetectMode;

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row. Required, not tidiness:
// the gate may run a whole shard with the knob set, and row 1 asserts the
// knob-OFF behaviour specifically. Restoring means the rows are order- and
// ambient-independent either way.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// THE KNOWN POSITIVE. 500 membership queries against `Array<Object>`; each one
// §8.6-boxes the `int` query and the box is never released (#4619, declined
// residual, recorded). Loop-amplified per CLAUDE.md's vacuous-oracle rule: a
// one-shot box can still be reachable from a live stack slot at exit and pass
// on both sides. Main returns 2 (elements 1 and 2 are both present).
constexpr const char* kLeak500 =
    "module M; class C public static method Main(): int "
    "  var a: Array<Object> := [1, 2]; "
    "  var hits: int := 0; "
    "  for var i in 1..500 do "
    "    if a.Contains(i) then hits := hits + 1; end; "
    "  end; "
    "  return hits; "
    "end Main; end C;";

// THE CONTROL. Byte-for-byte the same program with `Array<int>`: no §8.6
// boxing, so no box to leak. Same return value, same loop, same API — the only
// difference is the one the leak depends on.
constexpr const char* kClean500 =
    "module M; class C public static method Main(): int "
    "  var a: Array<int> := [1, 2]; "
    "  var hits: int := 0; "
    "  for var i in 1..500 do "
    "    if a.Contains(i) then hits := hits + 1; end; "
    "  end; "
    "  return hits; "
    "end Main; end C;";

bezel::codegen::CodegenOptions asanOpts() {
    bezel::codegen::CodegenOptions o{};
    o.sanitizeAddress = true;
    return o;
}

std::size_t leakCount() {
    return bezel::test::aotLeakDetections().load(std::memory_order_relaxed);
}

// The harness SKIPs (loudly, counted) when clang is absent; a row that asserted
// through that would be asserting on an unarmed oracle.
bool skipped(const JitResult& r) {
    return bezel::test::diagnosticMessageContains(r, "SKIP");
}

}  // namespace

// ── 1. OFF is blind: the pre-fix behaviour, pinned ─────────────────────────
// The "green with detection off" half of the proof. Generated-code ASan is ON
// here, so this row also establishes that ASan-without-LSan is exactly what
// `--full` was giving every row in the suite.
BEZEL_TEST(leak_detection_4638, detection_off_is_blind_to_the_known_leak) {
    LeakEnv env(nullptr);
    auto r = bezel::test::runAot(kLeak500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── 2. STRICT fails on the known positive — the point of the file ──────────
// The row watches the oracle fail a program and must itself report green, so
// it holds a `LeakExpectation` — the same stack-scoped interception
// `aot_infra_loud_3007_test.cpp` uses for the #3007 trip, and the only
// sanctioned use of it. `exp.count` is the load-bearing assertion: it is the
// harness's own record that the row WOULD have been failed, independently of
// what this body asserts about the diagnostic.
BEZEL_TEST(leak_detection_4638, strict_mode_fails_the_known_leak) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kLeak500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{1});
    BEZEL_EXPECT_TRUE(exp.lastFailing);
    BEZEL_EXPECT_EQ(exp.lastBytes, 12000ull);
    BEZEL_EXPECT_EQ(exp.lastAllocs, 500ull);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The SPECIFIC diagnostic, not merely "some error": a compile reject, a
    // crash and a timeout all set hasErrors() too, and every one of them would
    // satisfy a weaker assertion while proving nothing about leak detection.
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "LeakSanitizer fired"));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "byte(s) leaked in"));
}

// ── 3. STRICT stays green on the leak-free control ─────────────────────────
BEZEL_TEST(leak_detection_4638, strict_mode_passes_the_clean_control) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kClean500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── 4. REPORT records without changing the verdict ─────────────────────────
// `ran`, `exitCode` and `hasErrors()` must match row 1 EXACTLY — that identity
// is what lets the census run over the whole suite without turning a single row
// red — while the chokepoint counter proves the leak was nonetheless seen.
BEZEL_TEST(leak_detection_4638, report_mode_counts_without_changing_the_verdict) {
    LeakEnv env("report");
    const std::size_t before = leakCount();
    auto r = bezel::test::runAot(kLeak500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);          // the PROGRAM's exit status, not LSan's
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(leakCount(), before + 1);
}

// The control for row 4: a clean program must not move the counter, so row 4
// cannot be satisfied by a counter that ticks on every run.
BEZEL_TEST(leak_detection_4638, report_mode_does_not_count_a_clean_program) {
    LeakEnv env("report");
    const std::size_t before = leakCount();
    auto r = bezel::test::runAot(kClean500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(leakCount(), before);
}

// ── 5. leak-only vs real memory error ──────────────────────────────────────
// Unit-tested on report TEXT rather than on a hand-built use-after-free
// program: the property under test is the classifier, and a synthetic UAF in
// Bezel would pin whatever the compiler happens to emit today instead.
BEZEL_TEST(leak_detection_4638, leak_only_classifier_spec_28_2) {
    const std::string leakOnly =
        "=================================================================\n"
        "==1==ERROR: LeakSanitizer: detected memory leaks\n"
        "Direct leak of 12000 byte(s) in 500 object(s) allocated from:\n"
        "SUMMARY: AddressSanitizer: 12000 byte(s) leaked in 500 allocation(s).\n";
    const std::string useAfterFree =
        "==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x1\n"
        "SUMMARY: AddressSanitizer: heap-use-after-free\n";
    // A run that reports BOTH is a real memory error; Report mode must not
    // consume it.
    const std::string both = useAfterFree + leakOnly;

    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::hasLeakOnlyReport(leakOnly));
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasLeakOnlyReport(useAfterFree));
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasLeakOnlyReport(both));
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasLeakOnlyReport("clean\n"));
    // All three still satisfy the #1822 predicate — the point being that
    // `hasSanitizerReport` alone cannot make the distinction Report mode needs.
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::hasSanitizerReport(leakOnly));
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::hasSanitizerReport(useAfterFree));

    unsigned long long bytes = 0, allocs = 0;
    BEZEL_EXPECT_TRUE(
        bezel::test::aot_detail::parseLeakSummary(leakOnly, bytes, allocs));
    BEZEL_EXPECT_EQ(bytes, 12000ull);
    BEZEL_EXPECT_EQ(allocs, 500ull);
    // A malformed / absent SUMMARY must be reported as "no size", never as 0.
    unsigned long long b2 = 7, a2 = 7;
    BEZEL_EXPECT_FALSE(
        bezel::test::aot_detail::parseLeakSummary(useAfterFree, b2, a2));
    BEZEL_EXPECT_EQ(b2, 7ull);
}

// ── 6. mode parsing, and FAIL-CLOSED on a typo ─────────────────────────────
BEZEL_TEST(leak_detection_4638, mode_parsing_fails_closed_on_an_unknown_value) {
    {
        LeakEnv e(nullptr);
        BEZEL_EXPECT_TRUE(bezel::test::leakDetectMode() == LeakDetectMode::Off);
    }
    for (const char* off : {"0", "off", "false", ""}) {
        LeakEnv e(off);
        BEZEL_EXPECT_TRUE(bezel::test::leakDetectMode() == LeakDetectMode::Off);
    }
    for (const char* rep : {"report", "census"}) {
        LeakEnv e(rep);
        BEZEL_EXPECT_TRUE(bezel::test::leakDetectMode() == LeakDetectMode::Report);
    }
    for (const char* str : {"1", "strict", "on", "true"}) {
        LeakEnv e(str);
        BEZEL_EXPECT_TRUE(bezel::test::leakDetectMode() == LeakDetectMode::Strict);
    }
    // The shape of this whole issue is an oracle that is off while its operator
    // believes it is on. An unrecognised value therefore escalates to the
    // strictest reading rather than degrading to Off.
    for (const char* typo : {"yes", "leaks", "Strict", "ON"}) {
        LeakEnv e(typo);
        BEZEL_EXPECT_TRUE(bezel::test::leakDetectMode() == LeakDetectMode::Strict);
    }
}

// ── 7. the STRICT trip is real, and it is SCOPED ───────────────────────────
// Row 2 proves the oracle fires while an interception is in place. This row
// proves the two halves that interception hides: that with no LeakExpectation
// alive `noteLeakDetection` would mark the running test failed (the #3007
// shape — a leak must not be absorbed by a row that asserts `hasErrors()`),
// and that the interception is restored on scope exit so it cannot leak into
// the rest of the shard. Driven directly on the chokepoint, because doing it
// through a real run would necessarily fail this row by construction.
BEZEL_TEST(leak_detection_4638, strict_trip_is_real_and_scoped_spec_28_2) {
    const std::string report =
        "==1==ERROR: LeakSanitizer: detected memory leaks\n"
        "SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s).\n";
    // `currentTestFailed()` is BOTH this row's own verdict and the observable
    // under test, so every probe below SAVES it and RESTORES the saved value —
    // never clears it. Clearing would let a failed assertion earlier in the row
    // be erased by a later probe, which is a self-silencing oracle.
    auto probe = [&](bool failing) {
        const bool own = bezel::test::currentTestFailed();
        bezel::test::currentTestFailed() = false;
        bezel::test::aot_detail::noteLeakDetection("probe", report, failing);
        const bool tripped = bezel::test::currentTestFailed();
        bezel::test::currentTestFailed() = own;
        return tripped;
    };

    {
        bezel::test::aot_detail::LeakExpectation exp;
        BEZEL_EXPECT_FALSE(probe(/*failing=*/true));   // intercepted
        BEZEL_EXPECT_EQ(exp.count, std::size_t{1});
        BEZEL_EXPECT_TRUE(exp.lastFailing);
        BEZEL_EXPECT_EQ(exp.lastBytes, 24ull);
        BEZEL_EXPECT_EQ(exp.lastAllocs, 1ull);
    }
    // The interception is restored on scope exit; it cannot leak into the rest
    // of the shard (the ⚠ on InfraExpectation, which this mirrors).
    BEZEL_EXPECT_TRUE(bezel::test::aot_detail::currentLeakExpectation() ==
                      nullptr);
    // With nothing intercepting, STRICT trips for real — so a leak cannot be
    // absorbed by a row that merely asserts `hasErrors()` / `!ran`, which is
    // byte-for-byte the shape #3007 removed from the infra channel.
    BEZEL_EXPECT_TRUE(probe(/*failing=*/true));
    // REPORT never trips, interception or not.
    BEZEL_EXPECT_FALSE(probe(/*failing=*/false));
}
