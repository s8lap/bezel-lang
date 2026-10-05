// fix #3892: the ONE shared implementation of the "did this program fail at
// COMPILE TIME, not at runtime" discriminator, promoted out of two
// byte-identical per-file copies (tests/codegen/encoding_test.cpp, fix #3889;
// tests/codegen/file_io_644_test.cpp, fix #3891 — the reviewer of #3889 found
// #3891 had copy-pasted the first file's helper verbatim, which is exactly
// the "went from nonexistent to duplicated within one batch" evidence #3892
// records).
//
// WHY THIS EXISTS. `runAot`'s `JitResult::ran` is false for FOUR outcomes,
// not one (tests/support/aot_support.hpp):
//   1. a genuine `CompileOutcome::Rejected` — the driver's own captured
//      stderr, one real `<path>:<line>:<col>: <message>` line per diagnostic;
//   2. `CompileOutcome::Crashed` — "runAot: compiler CRASHED (signal N) …";
//   3. the program compiled and linked fine, then died from a runtime signal
//      — "runAot: program terminated by signal N …";
//   4. a compile or run timeout, or a forced-ASan report — "runAot: compile
//      did not finish within …", "runAot: program did not terminate within
//      …", "runAot: AddressSanitizer fired …".
// All four are reported as ordinary `Severity::Error` diagnostics at
// `SourceLocation{}` (0:0), so a row that pins "the compiler rejects this"
// with a bare `BEZEL_EXPECT_FALSE(r.ran)` (or `hasErrors()`, or a raw
// severity check) cannot tell outcome 1 apart from 2-4 — it ALSO passes when
// the program segfaults, hangs, or trips ASan. See issue #3892 for the
// measured blast radius (248 bare-`!ran`-shaped rows, 60 unprotected).
//
// ⚠ NAMING, NOT COSMETIC. This header deliberately does NOT reuse the name
// `hasErrorContaining` (or `hasError`, or `hasErrorWithRealLocation`) for its
// substring/location scanners, even though those are exactly the names the
// per-file copies used. Precisely one OTHER `tests/**/*_test.cpp` file
// (tests/codegen/encoding_getbytecount_span_1355_stdlib_3_2_test.cpp) already
// defines its own local `bool hasErrorContaining(const bezel::test::JitResult&,
// const char*)` in an anonymous namespace — the BYTE-IDENTICAL signature a
// same-named `bezel::test::hasErrorContaining` here would collide with.
// Verified with a two-line clang++ -std=c++20 repro (a same-signature copy in
// an anonymous namespace, called unqualified with a `bezel::test::JitResult`
// argument, alongside a same-named `bezel::test::` function): "call to
// 'hasErrorContaining' is ambiguous", because ADL always searches
// `JitResult`'s own namespace (bezel::test) for such an argument and merges
// into the local overload set.
//
// Three more files (tests/sema/{isarrlike_indexof_set_3765,sortedset_isarrlike
// _3730,reverse_set_3763}_test.cpp) define the SAME name over a JitResult but
// with `const std::string&` as the second parameter, not `const char*` —
// verified, ALSO by live clang++ compile, that this does NOT collide: called
// with a string-literal argument (the common case), exact-match overload
// resolution prefers the `const char*` candidate over the `std::string`
// one's user-defined conversion, so the call resolves cleanly rather than
// ambiguously. (That is a reason for caution, not safety, if a shared
// same-named function existed — a literal-argument call would SILENTLY
// prefer the newcomer over the file's own intended overload rather than
// erroring — but it is not the "ambiguous compile error" hazard the exact
// match produces.)
//
// The sibling name `hasErrorWithRealLocation(const JitResult&, const char*)`
// has this same exact-match hazard independently, between itself: it is
// ALREADY duplicated, near-identically, across
// tests/sema/chain_position_field_read_3829_test.cpp and
// tests/sema/sorted_dictionary_literal_target_removed_3831_test.cpp (a
// pre-existing defect this lane noticed but left alone — the two copies
// differ subtly in a `continue`-vs-`return false` branch on a
// no-location-found diagnostic, worth its own dedup issue).
//
// Counting every file across the family that shares a JitResult-shaped first
// parameter under one of these names — hasErrorContaining exact-match (1),
// hasErrorContaining string-second-param (3), hasErrorWithRealLocation (2) —
// plus the two further `hasErrorContaining(const bezel::test::OwnershipResult&,
// ...)` copies (tests/codegen/exception_surface_inherited_2971_test.cpp,
// tests/ownership/scoped_builtin_subscribe_1420_test.cpp — same NAME, a
// DIFFERENT result type with no ADL relevance to a JitResult-typed argument,
// cited only as further evidence of how widely this name is already reused)
// reaches 8 files, not the 14 an earlier revision of this comment claimed
// (that number conflated this family with unrelated `hasErrorContaining`/
// `hasError` copies over a `const std::string& program` — a source string,
// not a JitResult — which share the name but could never collide with
// anything defined over JitResult in the first place).
//
// Promoting under a fresh name (mirroring tests/support/run_under_asan.hpp's
// `runUnderAsanExitCode`, which sits beside many per-file `runUnderAsan`
// wrappers for the identical reason) sidesteps the hazard entirely: a caller
// that wants the old spelling keeps a one-line local forwarder (see
// encoding_test.cpp), everyone else is unaffected.
//
// HEADER-ONLY + `inline`: these are defined in a header included from many
// `tests/**/*_test.cpp` translation units across several test binaries — a
// non-`inline` free function here would be an ODR / multiple-definition link
// error (same discipline as run_under_asan.hpp).
#pragma once

#include <regex>
#include <string>

#include "support/codegen_support.hpp"   // bezel::test::JitResult

namespace bezel::test {

// True iff `r` was skipped for want of a toolchain (the ASan/LSan link could
// not run). Distinct from a genuine compile-time rejection, which is what makes
// it dangerous to act on: a SKIP means the memory-safety claim was never tested.
//
// WHY THIS EXISTS (#3733). A leak row written as
//
//     JitResult r = runAotChecked(prog);
//     if (wasSkipped(r)) return;              // <-- absorbing
//     BEZEL_EXPECT_TRUE(r.ran);
//
// returns WITHOUT asserting on a box whose clang cannot link ASan, so the whole
// sanitizer tier reports a clean PASS having run no sanitizer at all. That is
// the "silent skip" defect: the test is green, and it proved nothing. It is
// counted by scripts/check_toolchain_skip_surface.py as an *absorbing site*.
//
// The fix is not to delete the guard — a box genuinely without ASan should not
// be reported as a product failure — but to make the skip LOUD. `BEZEL_EXPECT_FALSE
// (checkedToolchainSkipped(r))` inverts the branch: the row still passes where
// the toolchain is absent, and it FAILS anywhere the skip is being used to
// paper over a real sanitizer finding, because such a finding is a rejection,
// not a SKIP.
//
// Callers must keep the guard as a separate statement from this predicate — the
// ratchet inspects a call whose bound result reaches a `return` on the strength
// of the literal "SKIP", so inlining the check into the `if` re-creates the
// absorbing shape this predicate is here to end.
inline bool checkedToolchainSkipped(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// True iff some diagnostic in `r` contains `sub`. The single scanning
// primitive both predicates below are built from.
inline bool diagnosticMessageContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// fix #4784 (§27.8, v0.307): how many times `sub` occurs across every
// diagnostic in `r`. `diagnosticMessageContains` only answers "at least
// one" — it cannot distinguish a diagnostic reported once from one reported
// twice, which is exactly the shape #4784's regression test needs to pin
// ("is recursive" must appear exactly once per distinct self-referencing
// occurrence, not once per re-entrant visit of it).
//
// Counts SUBSTRING occurrences, not matching `Diagnostic` entries: a genuine
// `CompileOutcome::Rejected` (the common case here) packs the driver's ENTIRE
// captured stderr — every `<path>:<line>:<col>: <message>` line, including
// more than one occurrence of the same wording — into a SINGLE `Diagnostic`
// at `SourceLocation{}` (see aot_support.hpp; `failedAtCompileTimeWithRealLocation`
// above already accounts for this by regex-scanning message TEXT rather than
// counting entries). A per-entry count would always read 0 or 1 for this
// path regardless of how many real diagnostic lines the compiler emitted,
// which would make it blind to exactly the duplicate-report defect #4784
// exists to catch.
inline int diagnosticMessageCount(const JitResult& r, const char* sub) {
    const std::string needle(sub);
    int count = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
    }
    return count;
}

// The STRONG claim: the driver itself rejected the program before ever
// executing it (the IR-verification-failure text both promoted call sites
// pin), never a runtime crash / signal / timeout / ASan report. `r.ran ==
// false` alone does NOT establish this — see the file header. Ported
// verbatim from encoding_test.cpp / file_io_644_test.cpp (fix #3889 /
// #3891); every existing call site names an "IR verification failed"
// diagnostic specifically, so that positive requirement is kept rather than
// generalised away. For a row that has no specific rejection text to pin,
// use `failedAtCompileTimeWithRealLocation` below instead.
inline bool failedAtCompileTimeNotRuntime(const JitResult& r) {
    return !r.ran &&
           diagnosticMessageContains(r, "IR verification failed") &&
           !diagnosticMessageContains(r, "terminated by signal") &&
           !diagnosticMessageContains(r, "compiler CRASHED");
}

// The LOCATION-based sibling, for a row that cannot pin specific message
// text (issue #3892's suggested remedy for such rows, generalising the idiom
// already used by tests/sema/instantiation_hash_container_diag_3796_test.cpp
// — `loc.line > 0 && loc.column > 0` — which is equally sound because both
// synthetic crash/timeout/signal/ASan diagnostics are pinned at
// `SourceLocation{}` and, unlike a genuine `CompileOutcome::Rejected`'s
// captured driver stderr, never render a real `<path>:<line>:<col>: `
// prefix in their message TEXT (Driver.cpp:632/644/773/785 is what prints
// that shape; aot_support.hpp's synthetic outcomes never call it).
//
// Belt-and-braces: rather than relying solely on "a real location prefix
// exists somewhere in the message" (a Crashed/TimedOut diagnostic ALSO
// appends whatever partial compiler stderr was captured before the crash/
// hang, via "\n--- compiler stderr ---\n" + err — see aot_support.hpp — and
// that captured fragment could itself carry an unrelated but real-looking
// location if the front end had printed anything before dying), this ALSO
// excludes any diagnostic whose text names one of the known non-reject
// outcomes explicitly, exactly like `failedAtCompileTimeNotRuntime` does.
inline bool failedAtCompileTimeWithRealLocation(const JitResult& r) {
    if (r.ran) return false;
    if (diagnosticMessageContains(r, "compiler CRASHED")) return false;
    if (diagnosticMessageContains(r, "terminated by signal")) return false;
    if (diagnosticMessageContains(r, "did not finish within")) return false;
    if (diagnosticMessageContains(r, "did not terminate within")) return false;
    if (diagnosticMessageContains(r, "AddressSanitizer fired")) return false;
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLoc) &&
            std::stoi(m[1].str()) > 0 && std::stoi(m[2].str()) > 0)
            return true;
    }
    return false;
}

}  // namespace bezel::test
