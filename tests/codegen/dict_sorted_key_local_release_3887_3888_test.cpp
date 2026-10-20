// Codegen — fix #3887 (indexer form) and fix #3888 (named-method form),
// stdlib §4.8/§4.9/§4.17, spec §28.9 (v0.294):
//
// `SortedDictionary<string, V>` is the receiver-type sibling of
// `Dictionary<string, V>`/`OwningDictionary<string, V>` for BOTH of the
// gaps fix #3872 (IndexExpr, `d[key]` get/set) and fix #3886 (walkCallArgs,
// `.ContainsKey`/`.TryGetValue`/`.Add`/`.TryAdd`/`.Remove`/`.GetOrNull`)
// already closed for the other two dict receivers. Both #3872 and #3886
// deliberately excluded `SortedDictionary` — a scope decision (narrow
// rather than widen speculatively), NOT a soundness boundary — and each
// pinned that exclusion in-suite with its own "SortedDictionary still
// leaks" control row. This file's job is the fourth cell of the 2x2
// (receiver family x walker arm) matrix those two issues named:
// SortedDictionary x {IndexExpr, walkCallArgs}.
//
// ROOT CAUSE: identical to #3872's/#3886's own, one receiver type over.
// `SortedDictionary<string,V>`'s built-in indexer carries no
// `PropertySymbol` (the #2717 user-indexer relaxation's gate) and its
// built-in named methods carry no resolved `sema::MethodSymbol` (they are
// matched structurally by name in `LlvmBackend_Calls.cpp`'s `isDict`
// dispatch block) — the SAME null-symbol shape #3872/#3886 already fixed
// for Dictionary/OwningDictionary, just excluded for this third receiver.
//
// SOUNDNESS, verified by reading the runtime and codegen dispatch (not
// inherited on faith — this file's own verification, independent of what
// #3886's reviewer established): `common::isDictionaryName`
// (`src/common/BuiltinRegistry.cpp`'s `kDictionaryNames`) is the SAME
// three-name registry (`Dictionary`, `OwningDictionary`, `SortedDictionary`)
// that `isDictTypeName` answers for every dict-family dispatch in
// `LlvmBackend_Calls.cpp` — there is no SortedDictionary-specific method or
// indexer arm anywhere in that dispatch; every one of ContainsKey/Contains/
// TryGetValue/Add/TryAdd/Remove/GetOrNull and the indexer get/set lowers
// through the IDENTICAL `bz_dict_*_str` runtime entry points regardless of
// receiver name. `src/runtime/bezel_runtime_collections.cpp` confirms
// SortedDictionary's §4.17 ordering machinery (`bz_dict_min_key_str`/
// `bz_dict_max_key_str`/the sorted-snapshot helpers used by iteration and
// `FirstKey`/`LastKey`) is a read-only O(n log n) SCAN taken over that same
// `bz_dict_t*` table — it never changes how a KEY is stored on insert.
// `bz_dict_set_str`/`bz_dict_set_str_disposing` `strndup` a byte COPY of the
// key into the table's own buffer and never retain or store the caller's
// `bz_string*` — the identical fact #3703/#3872/#3886 already rely on. So a
// scope-end release of the local's own +1 can never dangle the table's
// internal copy (no UAF) and never double-frees the same reference (the
// table never took one). SortedDictionary has no owning counterpart (stdlib
// §4.17: "There is no owning `OwningSortedDictionary` in this revision"),
// so there is no OwningSortedDictionary row to add here.
//
// THE FIX widens BOTH existing arms' `headName()` guards
// (`src/codegen/LlvmBackend_Setup.cpp`: `dictStrKeySafe` in the IndexExpr
// arm, `dictMethodStrKeySafe` in `walkCallArgs`) to also match
// `SortedDictionary`, carrying the SAME `isTargetIdent(...)` + plain-
// argument-shape guard both arms already require — no third mechanism.
//
// BOUNDARY, deliberately NOT claimed by this fix (a genuine control, not a
// fix-dependent row): the VALUE-position argument of `.Add`/`.TryAdd`
// (index 1) is EXCLUDED by both #3886's and this fix's `i == 0` gate,
// exactly as #3886 scoped it — a string local reused as the VALUE argument
// of `SortedDictionary<string,string>.Add(key, value)` still leaks, on
// BOTH sides of this commit. See `value_position_arg_still_leaks_
// control_28_9` below.
//
// FAIL-WITHOUT / PASS-WITH (this file, filter
// `dict_sorted_key_local_release_3887_3888`, bezel_tests_codegen --area
// codegen):
//   PASS-WITH (this commit): 11 passed, 0 failed (the VALUE-position
//   control passes too — it correctly still leaks).
//   FAIL-WITHOUT (src/codegen/LlvmBackend_Setup.cpp reverted to this
//   branch's pre-fix HEAD, bezel_tests_codegen RELINKED via the same
//   `--area codegen` build before rerunning — not just bezelc): the ten
//   SortedDictionary fix-dependent rows fail with a genuine `r.ran ==
//   false` ASan/LSan sanitizer report; the VALUE-position control passes
//   unchanged (it was never claimed by this fix on either side).

#include <string>

#include "support/aot_result_asserts.hpp"   // fix #3895: diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::diagnosticMessageContains;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 10 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 10 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// "shared" + digits(j): 7 chars for j in [0,9], 8 for [10,99], 9 for [100,199].
int keyLen(int j) { return 6 + ((j < 10) ? 1 : (j < 100 ? 2 : 3)); }

}  // namespace

// ---------------------------------------------------------------------------
// fix #3887, stdlib §4.8/§4.17: the indexer-SET form, mirroring #3872's own
// repro exactly but with a `SortedDictionary<string,int>` receiver.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           indexer_set_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3887, stdlib §4.8/§4.17: indexer GET after SET — exercises the
// walker's shared get/set `IndexExpr` arm from both directions; a strict
// ASan/LSan `runAotChecked` catches a UAF on the read, not just a leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           indexer_set_and_get_named_key_local_released_4_8) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  var v := d[key]; "
        "  total := total + key.Length + v; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j) + 1;
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17: `.ContainsKey(key)` — issue #3888's own
// repro, verbatim (indexer-set followed by ContainsKey).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           contains_key_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  if d.ContainsKey(key) then total := total + 1; end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += 1 + keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17: `.TryGetValue(key, out v)` — the
// nullable-out decode path, exercised from the same walker arm as
// ContainsKey above.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           try_get_value_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  var v: int? := null; "
        "  if d.TryGetValue(key, out v) then total := total + v!; end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += 1 + keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17: `.Add(key, value)` called directly as a
// METHOD (no indexer anywhere in this program) — isolates the
// walkCallArgs-arm gap from #3887's already-fixed IndexExpr-arm gap
// entirely; a fresh dict per iteration means the key is never a duplicate,
// so `.Add` never takes its ArgumentException edge (stdlib §4.17, v0.282).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           add_method_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d.Add(key, 1); "
        "  total := total + key.Length + d[key]; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j) + 1;
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17 (v0.282): `.TryAdd(key, value)` — the
// no-throw sibling of `.Add`.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           try_add_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  var added := d.TryAdd(key, 1); "
        "  if added then total := total + 1; end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += 1 + keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17: `.Remove(key)` — the key's sole read is the
// removal probe; `key` must stay live and correct for the `.Length` read
// afterward.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           remove_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  var removed := d.Remove(key); "
        "  if removed then total := total + 1; end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += 1 + keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// fix #3888, stdlib §4.8/§4.17 (v0.231 *OrNull tier): `.GetOrNull(key)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           get_or_null_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  total := total + key.Length + (d.GetOrNull(key) ?? 0); "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j) + 1;
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// COMBINED, strict oracle: the SAME named key local reused across an
// indexer-set (#3887's arm), ContainsKey and TryGetValue (#3888's arm), and
// the `in` operator (already safe before either fix — the walker's generic
// BinaryExpr arm treats both operands of every binary operator, `in`
// included, as non-escaping reads, independent of receiver type). Mirrors
// #3886's own combined row, receiver swapped to SortedDictionary. With
// every use on this list now a safe read for THIS receiver too, the row is
// asserted on the STRICT oracle (`runAotChecked`) as live, in-suite proof
// the combination is clean end-to-end for SortedDictionary, not merely each
// call in isolation.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           combined_indexer_containskey_in_trygetvalue_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var total := 0; var j := 0; "
        "while j < 200 do "
        "  using var d := new SortedDictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := 1; "
        "  if d.ContainsKey(key) then total := total + 1; end; "
        "  if key in d then total := total + 1; end; "
        "  var v: int? := null; "
        "  if d.TryGetValue(key, out v) then total := total + v!; end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j)
        expected += 1 /* ContainsKey */ + 1 /* in */ + 1 /* TryGetValue value */ +
                    keyLen(j);
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL, not a fix-dependent row: the VALUE-position argument
// (index 1) of `.Add(key, value)` is deliberately NOT widened by either
// walker arm's `i == 0` gate — exactly as issue #3886 scoped the
// Dictionary/OwningDictionary form, and unaffected by this fix's
// SortedDictionary widening (the widening only touches WHICH receiver
// types match at the KEY position, never which argument index does). A
// string local reused as the VALUE argument of
// `SortedDictionary<string,string>.Add(key, value)` still leaks its own +1,
// deliberately: this is a genuine control, asserting the SAME outcome (a
// real ASan/LSan leak report, `r.ran == false`) on BOTH sides of this
// commit.
//
// Measured via `scripts/agent_check.sh --probe --asan`, 50x: 1790 byte(s)
// leaked in 100 allocation(s) (1600 B / 50 objects direct + 190 B / 50
// objects indirect — 50 header+data pairs; one iteration's `val` survives
// past the loop under LSan's process-exit snapshot, matching the off-by-one
// accounting of every fix-dependent row above).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_sorted_key_local_release_3887_3888,
           value_position_arg_still_leaks_control_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 50 do "
        "  using var d := new SortedDictionary<string, string>(); "
        "  var val := $\"v{j}\"; "
        "  d.Add($\"k{j}\", val); "
        "  total := total + val.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Deliberately the INVERSE of every row above: the VALUE position is NOT
    // claimed by this fix, so it must still trip LeakSanitizer on both sides
    // of this commit — a leak report here is the expected, unchanging result.
    //
    // fix #3895 (issue #3892): pin that the failure is SPECIFICALLY the
    // expected leak report, not a compiler crash / unrelated runtime signal
    // that a bare `!ran` cannot tell apart from the leak this row wants.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "LSan/ASan at exit"));
}
