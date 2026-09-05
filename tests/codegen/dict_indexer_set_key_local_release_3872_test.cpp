// Codegen — fix #3872 / stdlib §4.8, §4.9 / spec §28.9 (v0.294):
// a named `string` LOCAL's own scope-end release was lost specifically when
// that local was passed as the KEY argument to a `Dictionary<string, V>` /
// `OwningDictionary<string, V>` indexer-SET (`d[key] := v`).
//
// Discovered while writing #3703's regression tests (a different,
// already-fixed dictionary-KEY leak: see dict_string_key_leak_3703_test.cpp).
// This is a SEPARATE, pre-existing defect: it reproduces identically on the
// unmodified base and is unaffected by #3703's own fix — #3703's release
// only ever fires for a KEY EXPRESSION that is itself a fresh +1 producer at
// the CALL SITE (`isPlusOneStringProducer` on the argument expression, e.g.
// a bare `$"...{i}"` interpolation written directly into `d[$"...{i}"]`);
// `key` here is a plain `IdentifierExpr` naming an already-bound local, which
// `isPlusOneStringProducer` correctly does not recognize, so #3703's release
// code never ran for this shape at all.
//
// ROOT CAUSE (traced by reading the escape-analysis walker, not guessed):
// a `var key := $"...";` local is classified a +1-PRODUCER candidate by
// `Emitter::collectStringReleaseEligible` (LlvmBackend_Setup.cpp) and is
// scope-end-release ELIGIBLE only if `StringEscapeWalker` proves it never
// escapes. That walker's `IndexExpr` arm (`expr()`, the `s[i]` case) already
// relaxes a bare-identifier INDEX ARGUMENT to a safe (non-escaping) read —
// but ONLY for a USER-DEFINED class/interface indexer, gated on
// `ctx.exprSymbol(*ix)` resolving to a `PropertySymbol` with `ps->isIndexer`
// (see #2717's citation in that arm). A BUILT-IN collection indexer —
// `Dictionary<string,V>`/`OwningDictionary<string,V>` — has NO PropertySymbol
// at all: ExprTypecheck_Exprs.cpp's own comment on the #2530 attachment says
// it is set "ONLY for the user-defined indexer branch (an
// Array<T>/OwningDictionary<>/etc. collection returns no PropertySymbol
// here)". So `idxPs` was always null for `d[key] := v`, every index position
// fell through to the walker's conservative `safeIdx=false`, and `key` was
// unconditionally marked ESCAPED — which suppresses its scope-end release
// (a safe-direction leak, never a double-free, but still a real one).
//
// THE FIX teaches the SAME walker arm a Dictionary/OwningDictionary-specific
// twin of the existing class-indexer relaxation: a bare string-local index
// argument at position 0 of a Dictionary<string,V>/OwningDictionary<string,V>
// receiver (any `V`) is ALSO a safe read, when `stringReleaseMode` is set
// (the same #1997 narrowing every other relaxation in this walker uses). The
// new arm is gated the SAME way the #2717 arm it twins is: plain argument
// shape (no modifier/name/discard) AND `isTargetIdent(...)` — the key
// expression must itself be the tracked local, not merely occupy the key
// position (review noted that today's `expr()` dispatch makes the
// `isTargetIdent` half of that gate observationally inert — only
// IdentifierExpr/PostfixBangExpr/ConditionalExpr consult an incoming
// `safeRead` at all, and a nullable `string?` local is never a release
// candidate in the first place — but it is restored as DEFENSIVE symmetry
// with the twinned arm rather than relying on that holding everywhere this
// arm could ever be reached).
//
// BOUNDARY (what this fix deliberately does NOT claim): `SortedDictionary
// <string,V>` has the textually identical `IndexExpr`/no-PropertySymbol
// shape (same null `idxPs`, same #3872-class root cause) but is NOT matched
// by this fix's `headName() == "Dictionary" || "OwningDictionary"` check, so
// a string local reused as a `SortedDictionary` indexer-set key keeps
// leaking exactly as before. `sorted_dictionary_indexer_set_key_local_still_
// leaks_control_28_9` below pins that boundary explicitly — tracked
// separately (a sibling issue covering `SortedDictionary` specifically).
//
// SOUNDNESS (why this can never double-free or UAF): the runtime's
// string-keyed table mode never retains or stores the caller's `bz_string*`
// for a KEY — `bz_dict_set_str` / `bz_dict_set_str_disposing`
// (src/runtime/bezel_runtime_collections.cpp) read only `(kd, kl)` — the
// key's data pointer and length — and, on a fresh insertion, `strndup` a
// BYTE COPY into the table's own malloc'd buffer. This is the exact fact
// #3703's fix already established and relies on for the KEY-PRODUCER release
// half; #3872 is its "already-bound local, not a producer expression"
// sibling. So a scope-end release of `key`'s own +1 can never dangle the
// dict's internal copy (the dict holds none of `key`'s bytes by reference)
// and never double-frees the same reference (the dict never took one).
//
// Repro (issue #3872's own, verbatim): a bare string local, never used as a
// dict key, is LSan-clean (`dict_string_key_leak_3703_test.cpp`'s sibling
// suite establishes this baseline elsewhere; not re-pinned here). The SAME
// local, additionally stored once via `d[key] := 1`, leaked 8248 B / 398
// allocations at 200x on the unmodified base (measured via
// `scripts/agent_check.sh --probe --asan`, independent of and prior to this
// gtest file) — 199 objects' worth of header+data (one iteration's `key`
// survives past the loop and is reclaimed at Main's own return, so 199 not
// 200 leak under LSan's process-exit snapshot).
//
// The `SortedDictionary` control row is measured at its own scale (50x, not
// 200x): 2040 B / 100 allocations leaked (1600 B / 50 direct + 440 B / 50
// indirect — 50 header+data pairs), confirmed via `scripts/agent_check.sh
// --probe --asan` on the tree carrying this fix. It is NOT fix-dependent —
// unlike the three rows above, it asserts the SAME outcome (still leaking)
// on both sides of this commit, which is what makes it a control rather
// than a fourth FAIL-WITHOUT row.
//
// FAIL-WITHOUT / PASS-WITH (this file, filter `dict_indexer_set_key_local_
// release_3872`, bezel_tests_codegen --area codegen):
//   PASS-WITH (this commit): 4 passed, 0 failed (the control passes here
//   too — SortedDictionary correctly still leaks).
//   FAIL-WITHOUT (src/codegen/LlvmBackend_Setup.cpp reverted to its
//   pre-fix HEAD, bezel_tests_codegen RELINKED via the same `--area codegen`
//   build before rerunning — not just bezelc): 1 passed, 3 failed — the
//   three Dictionary/OwningDictionary rows fail with a genuine `r.ran ==
//   false` ASan/LSan sanitizer report (not a vacuous pass); the
//   SortedDictionary control PASSES unchanged (it was never leak-free on
//   either side, so reverting the fix does not flip it — proving the
//   control does not silently ride along with the fix). Restored the fix
//   and relinked again before continuing.
//
// UPDATE (fix #3887, later commit): the SortedDictionary row this
// FAIL-WITHOUT/PASS-WITH count describes was accurate as of #3872's own
// commit — SortedDictionary genuinely was NOT claimed by that fix. Fix
// #3887 subsequently widened the SAME guard to also cover SortedDictionary,
// which flips that row (see its own comment, below, for the current
// FAIL-WITHOUT/PASS-WITH count under fix #3887). This historical note is
// left as originally written rather than rewritten, per this project's
// pinned-history convention; it no longer describes this file's current
// row count for a fresh run.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent — mirrors dict_string_key_leak_3703_test.cpp / dict_family_tryadd_
// 3372_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// A resource class for the OwningDictionary-side test: a static counter
// tracks live-vs-disposed and a Tag field carries a per-instance value.
// Mirrors dict_string_key_leak_3703_test.cpp's `progR` exactly.
std::string progR(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 / spec §28.9: the issue's own repro, verbatim — a named
// `string` local bound to a fresh interpolation, used ONLY as the indexer-SET
// KEY argument (never re-stored, never passed anywhere else), then read
// again via `.Length` after the store (proving the local is still LIVE and
// readable — this is not a case where the local happens to go dead early).
// FAIL-WITHOUT: LSan reports ~8248 B / 398 allocations at 200x (199
// iterations' worth of header+data; the final iteration's `key` is still
// live at process exit under LSan's snapshot). PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_indexer_set_key_local_release_3872,
           indexer_set_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "  // fresh +1 producer, bound to a local
        "  d[key] := 1; "
        "  total := total + key.Length; "  // key must still be live afterward
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // "shared" + digits(j): 7 chars for j in [0,9], 8 for [10,99], 9 for [100,199].
    int expected = 0;
    for (int j = 0; j < 200; ++j) {
        int digits = (j < 10) ? 1 : (j < 100 ? 2 : 3);
        expected += 6 + digits;
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.8: the same named key local, additionally read BACK out of the
// dictionary via the indexer GET (`d[key]`) after the store — exercises the
// walker's shared get/set `IndexExpr` arm from both directions and confirms
// the relaxation does not corrupt or dangle the VALUE side (a strict
// ASan/LSan `runAotChecked` catches a UAF on the read, not just a leak).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_indexer_set_key_local_release_3872,
           indexer_set_and_get_named_key_local_released_4_8) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  var v := d[key]; "
        "  total := total + key.Length + v; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) {
        int digits = (j < 10) ? 1 : (j < 100 ? 2 : 3);
        expected += 6 + digits + 1;  // + the stored value (always 1)
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / spec §28.11: the OwningDictionary<string,V> twin — the KEY
// side of the fix is identical for the owning family (its runtime key
// handling is byte-for-byte the same `bz_dict_set_str[_disposing]` call;
// only the VALUE side differs). Uses a resource class `R` so a disposal-order
// UAF on the VALUE side (which this fix does not touch) would also surface:
// every stored `R` must be disposed exactly once (by the dict's own Dispose
// at `using var` scope end), independent of the key-local release this test
// targets.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_indexer_set_key_local_release_3872,
           owning_dictionary_indexer_set_named_key_local_released_4_9) {
    JitResult r = runAotChecked(progR(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := new R(j); "
        "  total := total + key.Length + d[key].Tag; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return R._n; "));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) {
        int digits = (j < 10) ? 1 : (j < 100 ? 2 : 3);
        expected += 6 + digits + j;
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
    BEZEL_EXPECT_EQ(r.exitCode, 200);  // every R disposed exactly once
}

// ---------------------------------------------------------------------------
// UPDATED by fix #3887 (§4.8/§4.17/§28.9, v0.294) — this row PINNED THE
// OPPOSITE OUTCOME from #3872 through fix #3887: `SortedDictionary<string,V>`
// had the textually identical shape that caused #3872 (the same `IndexExpr`
// walker arm, the same null `ctx.exprSymbol(*ix)` — a built-in collection
// indexer carries no `PropertySymbol` for ANY collection family, not only
// Dictionary/OwningDictionary) but #3872's own guard was
// `headName() == "Dictionary" || "OwningDictionary"` only, deliberately
// scoped to the two families that issue named. `SortedDictionary`'s own
// instance of that defect class was filed as issue #3887 and is now FIXED:
// `dictStrKeySafe`'s guard (LlvmBackend_Setup.cpp) widened to also match
// `SortedDictionary`, gated on the identical soundness argument (the
// runtime's `bz_dict_set_str`/`bz_dict_try_get_str` never retain or store
// the caller's `bz_string*` for a KEY regardless of receiver name — see
// `dict_sorted_key_local_release_3887_3888_test.cpp` for the full
// verification and the fresh boundary this fix does NOT claim).
//
// This row previously asserted the INVERSE (`r.ran == false`, a genuine
// leak control) on both sides of #3872's commit. Fix #3887 flips that
// outcome for real, so the row is updated in the SAME commit that lands
// #3887 (per this project's "a fix reversing a prior rejection/outcome must
// update the tests that pinned the OLD behaviour" rule) rather than left
// pinned to a now-stale expectation.
//
// FAIL-WITHOUT / PASS-WITH (this row specifically, as part of fix #3887's
// commit): PASS-WITH — clean, `total=390`. FAIL-WITHOUT (src/codegen/
// LlvmBackend_Setup.cpp reverted to the pre-#3887 HEAD, bezel_tests_codegen
// RELINKED before rerunning): `r.ran == false`, an LSan report of 1999
// byte(s) / 98 allocation(s) at this row's own 50x scale (measured on this
// tree; the issue's originally-filed number of 2040 B / 100 allocations was
// measured against a slightly different tree revision — same signature,
// same off-by-one accounting, the discrepancy is immaterial to the fix).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_indexer_set_key_local_release_3872,
           sorted_dictionary_indexer_set_key_local_now_released_3887) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 50 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 50; ++j) {
        int digits = (j < 10) ? 1 : 2;
        expected += 6 + digits;
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}
