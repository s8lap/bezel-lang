// Codegen — fix #4127 (§27.5 Closures / §28.9 Runtime-managed types, stdlib
// §4.8 DictionaryEntry<K,V>, v0.295): a string-keyed `DictionaryEntry<string,
// V>` local bound directly to `d.First()`/`.Last()`/`.Single()`/
// `.ElementAt(i)` and captured by a closure that ESCAPES (is directly
// returned) never had its rebuilt `bz_string*` Key released — fix #3720's
// own documented trade-off: `isFreshDictEntryKeyReleaseTerminalLocal`'s
// eligibility walk (StringEscapeWalker) treats ANY capture as an escape and
// so never registers the local's scope-end `dictEntryKeyRelease` disposal,
// and nothing else ever discharged the rebuilt Key either.
//
// MEASURED (500x amplified under ASan/LSan; scripts/agent_check.sh --probe
// --asan), on the parent commit (a9ad986, i.e. WITH fix #4129 but WITHOUT
// this fix), the issue's own repro shape:
//
//   var d := new Dictionary<string, int>();
//   d.Add("xy", 9);
//   var fe := d.First();
//   return lambda() => fe.Key.Length + fe.Value;
//
//   total=5500
//   SUMMARY: AddressSanitizer: 17500 byte(s) leaked in 1000 allocation(s).
//
// — exactly 35 B / 2 allocations per call (32 B calloc'd `bz_string_t`
// header + 3 B malloc'd "xy\0" payload), matching #4127's own single-call
// measurement (32+3 B) exactly. The program's own value (total=5500 =
// 500 * 11) is correct on both sides — a pure leak, never a UAF or
// double-free.
//
// THE FIX. `lowerLambda`'s per-lambda envdtor (LlvmBackend_Exprs2.cpp)
// already frees a captured local's boxed CELL when — and only when — that
// local's cell has EXACTLY ONE capturing closure and that closure
// DEFINITELY escapes (`ce.freeCellInEnvdtor`, §27.5/#694: singleCaptureLocals_
// ∧ definitelyEscapingLambdas_ — the "single-holder, definite-escape" proof
// that makes freeing the cell sound: no other env can double-free it, and a
// definitely-escaping (directly-returned) closure's envdtor cannot run until
// strictly after this frame is gone, so there is no later read through the
// cell within this function to race). A `DictionaryEntry<string,V>`'s cell
// holds the `{ptr Key, V Value}` struct BY VALUE, with Key at field 0 (byte
// offset 0) — the SAME shape a plain captured `string` local's cell has, so
// releasing it needs no new struct-layout knowledge: a bare `ptr`-sized load
// off the cell's base address reads the Key exactly as the existing
// string-capture branch reads a plain string. New branch,
// `isDictEntryStringKeyTypeRef(ce.semaTy)` (LlvmBackend_impl.hpp), gates a
// `bz_string_release` on that load, ADDITIONALLY to (before) the existing
// cell free — guarded by the identical `ce.freeCellInEnvdtor` proof, so no
// new retain-balancing question arises (singleCaptureLocals_ guarantees no
// second capture shares the +1).
//
// SCOPE. Deliberately NOT extended to the for-loop-VARIABLE sibling
// (`for var entry in d do return lambda() => entry.Key...; end;`,
// dict_entry_closure_capture_3876_test.cpp's
// `for_loop_variable_string_key_capture_no_stack_overflow`): `!isForVar` is
// a HARD PRECONDITION of `freeCellInEnvdtor` itself (a for-var's boxed cell
// is fresh per iteration, so no single envdtor could be its sole reclaim
// site) — that shape stays on the pre-existing safe-direction leak,
// unaffected by this fix; see #4129/#4161. Also NOT extended to
// `DrainedEntry<K,V>` (#4135's sibling issue, a different lowering path with
// a different, larger owned-field set) or to a NON-escaping capture (never
// reaches `freeCellInEnvdtor` at all — its cell, if reclaimable, goes
// through the scope-end `cellFreeEligible_`/`boxedCellFree` route instead,
// unaffected by this change).
//
// ORACLE. `runAotChecked` (ASan+LSan) loop-amplifies every leak-checking row
// 500x per LANE_RULES so a one-shot pass (which would pass on BOTH sides —
// the leaked allocation is still stack-reachable at process exit for a
// single call) cannot masquerade as a fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

std::string makeClosureProgram(const std::string& declKw,
                                const std::string& terminal) {
    return
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); " +
        declKw + " fe := d." + terminal + "; "
        "    return lambda() => fe.Key.Length + fe.Value; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do "
        "      var f := C.MakeClosure(); "
        "      total := total + f(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Value correctness (plain runAot — unaffected by this fix either way).
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim (single-shot, no amplification): `d.First()`
// over a two-entry dict inserted in order "a" then "xy" — First() is "a"
// (Key.Length=1) + Value=1 = 2.
BEZEL_TEST(dict_entry_key_release_capture_envdtor_4127,
           first_captured_escaping_closure_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
        "    var fe := d.First(); "
        "    return lambda() => fe.Key.Length + fe.Value; "
        "  end MakeClosure; "
        "  public static method Main(): int return C.MakeClosure()(); end Main; "
        "end C;"),
        2);
}

// ---------------------------------------------------------------------------
// Leak coverage (runAotChecked, 500x amplified) — the FAIL-WITHOUT / PASS-
// WITH axis this fix is actually about.
// ---------------------------------------------------------------------------

// #4127 core repro, `var` binding, 500x. FAIL-WITHOUT: 17500 B / 1000
// allocations (35 B / 2 allocations per call, measured above). PASS-WITH:
// clean. "a"(1) + "xy"(9) inserted in that order; First() is "a" ->
// Key.Length=1 + Value=1 = 2, so total = 500*2 = 1000.
BEZEL_TEST(dict_entry_key_release_capture_envdtor_4127,
           var_first_captured_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeClosureProgram("var", "First()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// `const` twin of the row above — both flavors bind through the identical
// §21.1 LocalSymbol machinery `localSlot`/`boxedStorage` key on (per this
// file's sibling `dict_entry_closure_capture_3876_test.cpp`'s own note), so
// both exercise the same fixed code path. "a" + "xy" inserted in that
// order; total = 500*2 = 1000.
BEZEL_TEST(dict_entry_key_release_capture_envdtor_4127,
           const_first_captured_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeClosureProgram("const", "First()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// `.Last()` twin — a DIFFERENT terminal reaching the same
// isFreshDictEntryKeyReleaseTerminalLocal eligibility gate (First/Last/
// Single/ElementAt all rebuild the Key identically per #3720). Last() over
// {"a":1, "xy":9} is "xy" -> Key.Length=2 + Value=9 = 11, total = 500*11 =
// 5500.
BEZEL_TEST(dict_entry_key_release_capture_envdtor_4127,
           var_last_captured_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeClosureProgram("var", "Last()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}

// ---------------------------------------------------------------------------
// Accept-side control: a NON-escaping closure (called inline, never
// returned) never reaches `freeCellInEnvdtor` at all (`definitelyEscapingLambdas_`
// requires a direct `return`), so this fix's new branch does not fire for
// it. Measured (500x): this shape ALSO leaks the rebuilt Key — 34 B / 2
// allocations per call (32 B header + "a\0" 2 B payload) — a SEPARATE,
// NOT-fixed-here gap (neither `dictEntryKeyRelease`'s scope-end release,
// declined because ANY capture reads as an escape to StringEscapeWalker,
// nor this fix's `freeCellInEnvdtor`-gated release, which requires a
// DEFINITE escape, ever discharges it). Filed as #4165. `runAotCheckedLenient`
// (LeakSanitizer off) so this row documents "still runs correctly, still
// leaks, unaffected by #4127" rather than silently passing or spuriously
// failing under the strict oracle used elsewhere in this file. "a"(1) +
// "xy"(9); First() is "a" -> Key.Length=1 + Value=1 = 2, total = 500*2 = 1000.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_key_release_capture_envdtor_4127,
           non_escaping_capture_unaffected_still_leaks_4165) {
    const std::string src =
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var fe := d.First(); "
        "    var f := lambda() => fe.Key.Length + fe.Value; "
        "    return f(); "
        "  end Compute; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Compute(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}
