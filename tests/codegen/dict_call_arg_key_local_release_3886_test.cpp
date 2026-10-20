// Codegen — fix #3886 / stdlib §4.8, §4.9 / spec §28.9 (v0.294, this
// worktree's base revision — verified against docs/Bezel_Language_Spec.md's
// and docs/Bezel_Standard_Library.md's own headers before citing):
//
// Built-in Dictionary/OwningDictionary NAMED-METHOD call arguments
// (.ContainsKey(key), .TryGetValue(key, out v), .Add(key, v), .TryAdd(key, v),
// .Remove(key), .PopValue(key), .TryPopValue(key, out v), .GetOrNull(key),
// .GetOrAdd(key, factory)) still escaped a string-local KEY, the walkCallArgs
// TWIN of the gap fix #3872 already closed for the INDEXER form (`d[key]`
// get/set), at a different StringEscapeWalker entry point.
//
// ROOT CAUSE (traced by reading the walker, not guessed, mirroring #3872's
// own method): `Emitter::collectStringReleaseEligible` (LlvmBackend_Setup.cpp)
// classifies a `var key := $"...";` local a +1-PRODUCER candidate, eligible
// for scope-end release only if `StringEscapeWalker` proves it never
// escapes. `walkCallArgs`'s existing relaxations (#1430/#2222/#2824/#1185/
// #1994/#3304) all require a resolved `sema::MethodSymbol`, a compiled FFI
// extern, or one of a hand-listed set of specific intrinsic builtins
// (StringBuilder.Append/AppendLine, Console.Write/WriteLine). None of them
// recognize a Dictionary/OwningDictionary built-in method's string KEY
// parameter position: these calls are matched STRUCTURALLY BY NAME in
// LlvmBackend_Calls.cpp's `isDict` dispatch block (`ms` stays null for every
// one, mirroring how a built-in indexer carries no PropertySymbol — the exact
// #3872 root cause, one walker arm over). So a bare string-local KEY argument
// to any of these calls fell through to the walker's conservative escape and
// `key`'s own scope-end release never armed.
//
// THE FIX adds a Dictionary/OwningDictionary-specific relaxation to
// `walkCallArgs` (src/codegen/LlvmBackend_Setup.cpp, `dictMethodStrKeySafe`):
// a bare string-local argument at KEY position (always index 0 — verified in
// LlvmBackend_Calls.cpp: every admitted method's `k = lowerExpr(*call.
// args[0].value)`) of a Dictionary<string,V>/OwningDictionary<string,V>
// receiver's ContainsKey/Contains/TryGetValue/Add/TryAdd/Remove/PopValue/
// TryPopValue/GetOrNull/GetOrAdd call is ALSO a safe read, gated on
// stringReleaseMode (the #1997 narrowing) AND the same plain-argument-shape +
// isTargetIdent(...) guard every other relaxation in this walker uses.
//
// SOUNDNESS (per method, verified by reading LlvmBackend_Calls.cpp rather
// than assumed): every one of the ten admitted methods lowers its string KEY
// through `strDataLen(k)` — extracting only the (data, length) byte pair;
// on a fresh insertion `bz_dict_set_str` `strndup`s a COPY into the table's
// own buffer. None of them stores or retains the caller's `bz_string*`
// itself — the identical fact #3703's and #3872's fixes already established.
// So a scope-end release of the local's own +1 can never dangle the dict's
// internal copy (no UAF) and never double-frees the same reference (the dict
// never took one). The VALUE-position argument (index 1 of Add/TryAdd) and
// GetOrAdd's factory-closure argument (also index 1) are NOT widened — the
// fix's `i == 0` gate excludes them; their ownership story differs (§3703's
// VALUE-side handling) and stays out of scope, exactly as the issue names.
//
// BOUNDARY, pinned in-suite: `SortedDictionary<string,V>` has the textually
// identical null-resolved-symbol shape but is NOT matched (guard is
// headName()=="Dictionary"||"OwningDictionary" only) — #3872's own boundary
// control already established this for the indexer form; this fix does not
// touch SortedDictionary either, and `sorted_dictionary_contains_key_still_
// leaks_control_28_9` below pins it explicitly for the NAMED-METHOD form.
//
// Repro (issue #3886's own, verbatim, 200x under ASan/LSan): adding
// `d.ContainsKey(key)` after `d[key] := 1` (on a tree already carrying #3872's
// indexer fix, where the indexer-only form is clean) reintroduces the
// identical leak signature — 8290 B / 400 allocations at 200x.
//
// FAIL-WITHOUT / PASS-WITH (this file, filter
// `dict_call_arg_key_local_release_3886`, bezel_tests_codegen --area codegen):
//   PASS-WITH (this commit): 11 passed, 0 failed (the SortedDictionary
//   control passes too — it correctly still leaks).
//   FAIL-WITHOUT (src/codegen/LlvmBackend_Setup.cpp reverted to this branch's
//   pre-fix HEAD, bezel_tests_codegen RELINKED via the same `--area codegen`
//   build before rerunning — not just bezelc): the nine Dictionary/
//   OwningDictionary fix-dependent rows below fail with a genuine `r.ran ==
//   false` ASan/LSan sanitizer report; the SortedDictionary control and the
//   combined multi-call row's leak-side both remain unaffected in direction
//   (see each test's own comment for exact counts).
//
// UPDATE (fixes #3887/#3888, later commit): the SortedDictionary row this
// count describes was accurate as of #3886's own commit. Fixes #3887/#3888
// subsequently widened the same two guards to also cover SortedDictionary,
// which flips that row (see its own comment, below, for the current
// FAIL-WITHOUT/PASS-WITH count). Left as originally written, per this
// project's pinned-history convention, rather than rewritten.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. Each of this file's eleven rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it.
// That is an ABSORBING site by scripts/check_toolchain_skip_surface.py's own
// definition, and it is a silent vacuous pass: on a box whose clang cannot
// link the ASan binary, all eleven rows returned WITHOUT asserting and the
// suite reported a clean PASS having compiled and run nothing. MEASURED on
// the pre-change tree with the harness's own knob
// (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): 11 passed, 0 failed, and
// `CHECKED TOOLCHAIN: 11 skip(s)` — eleven rows green, zero programs run.
//
// Each guard is replaced by the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r))`
// (tests/support/aot_result_asserts.hpp): the skip is still recognised as an
// environment condition, but it now MARKS THE ROW FAILED instead of hiding
// it. That is strictly stronger than the guard it replaces — the row can no
// longer report green on a machine that never compiled the program — and it
// leaves every substantive assertion in these eleven rows (r.ran, exitCode,
// stdoutText) exactly where it was, in the same order.

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// A resource class for the OwningDictionary-side tests: a static counter
// tracks live-vs-disposed and a Tag field carries a per-instance value.
// Mirrors dict_indexer_set_key_local_release_3872_test.cpp's `progR` exactly.
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

// "shared" + digits(j): 7 chars for j in [0,9], 8 for [10,99], 9 for [100,199].
int keyLen(int j) { return 6 + ((j < 10) ? 1 : (j < 100 ? 2 : 3)); }

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8: the issue's own repro, verbatim — ContainsKey after an
// indexer-set, with the named key local read again afterward (proving it is
// still LIVE and correct, not merely non-crashing).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           contains_key_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8: `.TryGetValue(key, out v)` — the out-parameter decode path,
// exercised from the SAME walker arm as ContainsKey above.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           try_get_value_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8: `.Add(key, value)` called directly as a METHOD (no indexer
// anywhere in this program) — isolates the walkCallArgs gap from #3872's
// already-fixed IndexExpr gap entirely; a fresh dict per iteration means the
// key is never a duplicate, so `.Add` never takes its ArgumentException edge.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           add_method_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8: `.TryAdd(key, value)` — the no-throw sibling of `.Add`,
// explicitly named in issue #3886's suggested fix shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           try_add_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8: `.Remove(key)` — the key's sole read is the removal probe;
// `key` must stay live and correct for the `.Length` read afterward.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           remove_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8 (v0.231 *OrNull tier): `.GetOrNull(key)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           get_or_null_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// stdlib §4.8: `.GetOrAdd(key, factory)` — the KEY position only; the
// factory closure is a SEPARATE argument (index 1) this fix does not widen
// (a non-capturing factory, so #3871's unrelated capturing-factory leak
// cannot interfere with this row's measurement).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           get_or_add_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "
        "  var v := d.GetOrAdd(key, lambda(k: string) => 1); "
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
// stdlib §4.9 / spec §28.11: `OwningDictionary<string, R>.PopValue(key)` —
// the owning family's KEY handling is byte-identical to the borrowing
// family's (only the VALUE side differs); uses a resource class so a
// disposal-order UAF on the VALUE side (untouched by this fix) would also
// surface via a wrong `R._n` disposal count.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           pop_value_owning_dictionary_named_key_local_released_4_9) {
    JitResult r = runAotChecked(progR(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := new R(j); "
        "  using var taken := d.PopValue(key); "
        "  total := total + key.Length + taken.Tag; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return R._n; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j) + j;
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
    BEZEL_EXPECT_EQ(r.exitCode, 200);  // every R disposed exactly once
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / spec §28.11: `OwningDictionary<string, R>.TryPopValue(key,
// out using var v)` — the TRY analogue; `out using var` scopes the popped
// R's disposal to the `if` body, independent of this fix's KEY-local release.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           try_pop_value_owning_dictionary_named_key_local_released_4_9) {
    JitResult r = runAotChecked(progR(
        "var j := 0; var total := 0; "
        "while j < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var key := $\"shared{j}\"; "
        "  d[key] := new R(j); "
        "  if d.TryPopValue(key, out using var v) then "
        "    total := total + v!.Tag; "
        "  end; "
        "  total := total + key.Length; "
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return R._n; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int j = 0; j < 200; ++j) expected += keyLen(j) + j;
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
    BEZEL_EXPECT_EQ(r.exitCode, 200);  // every R disposed exactly once
}

// ---------------------------------------------------------------------------
// COMBINED, strict oracle: the SAME named key local reused across an
// indexer-set, ContainsKey, the `in` operator, and TryGetValue — the exact
// shape `dict_string_key_leak_3703_test.cpp`'s
// `reused_key_local_not_prematurely_released_28_9` test routes around with
// `runAotCheckedLenient`, because — as that test's own comment records —
// "the walker's own escaped=true short-circuit means whichever of these runs
// first ... is what still suppresses the release; today that is
// ContainsKey". `key in d` was ALREADY safe before this fix (the walker's
// generic BinaryExpr arm treats both operands of every binary operator,
// including `in`, as non-escaping reads — verified in this same struct's
// `expr()` dispatch, a few lines below `walkCallArgs`); it was ContainsKey
// and TryGetValue — both now admitted by this fix — that kept `escaped`
// pinned `true` for the whole local. With every use on this list now a safe
// read, this row is asserted on the STRICT oracle (`runAotChecked`, not
// `..Lenient`) as live, in-suite proof that the combination is now clean —
// not merely each call in isolation.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           combined_containskey_in_trygetvalue_named_key_local_released_28_9) {
    JitResult r = runAotChecked(prog(
        "var total := 0; var j := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
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
// UPDATED by fix #3888 (§4.8/§4.17/§28.9, v0.294) — this row PINNED THE
// OPPOSITE OUTCOME through fix #3888: `SortedDictionary<string,V>` had the
// textually identical null-resolved-symbol shape (a built-in collection
// method carries no `sema::MethodSymbol` for ANY collection family, not
// only Dictionary/OwningDictionary) but fix #3886's own guard was
// `headName() == "Dictionary" || "OwningDictionary"` only, deliberately
// scoped to the two families that issue named — mirroring #3872's own
// indexer-side boundary. `SortedDictionary`'s own instance of this
// walkCallArgs-arm defect was filed as issue #3888 and is now FIXED:
// `dictMethodStrKeySafe`'s guard (LlvmBackend_Setup.cpp) widened to also
// match `SortedDictionary`, gated on the identical soundness argument
// (every admitted method dispatches through the same `bz_dict_*_str`
// runtime regardless of receiver name — see
// `dict_sorted_key_local_release_3887_3888_test.cpp` for the full
// verification and the fresh VALUE-position boundary this fix does NOT
// claim). Note this repro ALSO exercises the indexer-set form (`d[key] :=
// 1`) that issue #3887 fixed at the same time (this row combines both
// walker arms, exactly as originally written), so both fixes had to land
// together for it to go clean.
//
// This row previously asserted the INVERSE (`r.ran == false`, a genuine
// leak control) on both sides of #3886's commit. Fixes #3887/#3888 flip
// that outcome for real, so the row is updated in the SAME commit that
// lands them (per this project's "a fix reversing a prior rejection/outcome
// must update the tests that pinned the OLD behaviour" rule) rather than
// left pinned to a now-stale expectation.
//
// FAIL-WITHOUT / PASS-WITH (this row specifically, as part of fixes
// #3887/#3888's commit): PASS-WITH — clean, `total=440`. FAIL-WITHOUT
// (src/codegen/LlvmBackend_Setup.cpp reverted to the pre-#3887/#3888 HEAD,
// bezel_tests_codegen RELINKED before rerunning): `r.ran == false`, an LSan
// report of 2040 byte(s) / 100 allocation(s) at this row's own 50x scale
// (measured live, matches the issue's own filed number exactly).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_call_arg_key_local_release_3886,
           sorted_dictionary_contains_key_now_released_3888) {
    JitResult r = runAotChecked(prog(
        "var j := 0; var total := 0; "
        "while j < 50 do "
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
    for (int j = 0; j < 50; ++j) {
        int digits = (j < 10) ? 1 : 2;
        expected += 1 /* ContainsKey */ + 6 + digits;
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}
