// Codegen — fix #4165 (§27.5 Closures / §28.9 Runtime-managed types, stdlib
// §4.8 DictionaryEntry<K,V>, v0.295): a string-keyed `DictionaryEntry<string,
// V>` local bound directly to `d.First()`/`.Last()`/`.Single()`/
// `.ElementAt(i)` and captured by a NON-ESCAPING closure (declared, invoked
// inline, then dropped at the function's scope end) also leaks its rebuilt
// `bz_string*` Key — the parallel sibling of #4127's escaping-closure case.
//
// The escaping case is fix #4127. That fix added a `freeCellInEnvdtor`-gated
// `bz_string_release` to lowerLambda's envdtor body — but
// `freeCellInEnvdtor` requires `definitelyEscapingLambdas_`, so it never
// fires for a closure that does NOT escape.
//
// For non-escaping closures, `cellFreeEligible_` IS populated (the pre-scan
// proves every capturing closure dies before the frame), and
// `boxedCellFree` correctly fires at the OWNER's scope end to free the cell.
// But the Key was never released: `isFreshDictEntryKeyReleaseTerminalLocal`'s
// own StringEscapeWalker treats ANY closure capture as an escape, so the
// local was never registered in `dictEntryKeyReleaseEligible_` and its
// scope-end `dictEntryKeyRelease` disposal never fires. The
// `boxedCellFree` arm frees the cell (8 bytes), and the rebuilt Key leaks
// (32-byte calloc'd `bz_string_t` header + N-byte malloc'd payload, one of
// each per call — 32 + N bytes per call, 500 calls → 500 × (32 + N) bytes
// total).
//
// MEASURED (500x amplified under ASan/LSan; scripts/agent_check.sh --probe
// --asan) on the pre-fix HEAD (origin/main), the issue's own repro shape:
//
//   var d := new Dictionary<string, int>();
//   d.Add("xy", 9);
//   var fe := d.First();
//   var f := lambda() => fe.Key.Length + fe.Value;
//   return f();
//
//   total=5500
//   SUMMARY: AddressSanitizer: 17500 byte(s) leaked in 1000 allocation(s).
//
// — exactly 35 B / 2 allocations per call (32 B calloc'd `bz_string_t`
// header + 3 B malloc'd "xy\0" payload). The program's own value
// (total=5500 = 500 * 11) is correct on both sides — a pure leak, never a
// UAF or double-free.
//
// THE FIX. Where `boxedCellFree` is registered for a captured local in
// `consumeLastLambdaCapturedCells` (LlvmBackend_Setup.cpp) or
// `lowerLambda`'s `declaredShallower` arm (LlvmBackend_Exprs2.cpp), also
// set `dictEntryKeyReleaseDeferred = true` when the captured local is a
// string-keyed `DictionaryEntry<string,V>`. In `emitDisposals` FIRST PASS
// (the `boxedCellFree` arm, LlvmBackend_Disposal.cpp) and SECOND PASS (the
// `boxedCellFreeDeferred` arm), this flag emits the same GEP field 0 +
// `bz_string_release` as the plain `dictEntryKeyRelease` arm — firing at
// the identical point and with the identical safety case: the deferred
// entry is registered ONLY where `boxedCellFree[Deferred]` fires, which
// requires `singleCaptureLocals_` (exactly one holder, exactly one +1 to
// balance), and the second-pass ordering guarantees no reader remains
// (every env is released, every owner reads-through-cell disposal is
// complete).
//
// NARROW-FIX-FORWARD NOTE. The original commit (c1237c9f) was a NO-OP — the
// leak bytes were unchanged pre-/post-fix (17500 B / 1000 allocs both
// sides). Root cause: the registration stored the ORIGINAL local's struct
// alloca into `dictEntryForVarSlot` by reading `locals_[ce.key]`, but
// `lowerLambda` had ALREADY rebound `locals_[decl] = newSlot`
// (Exprs2.cpp:1664) to the boxed cell holder (a `ptr*` alloca) by the time
// `consumeLastLambdaCapturedCells` ran. The emission's
// `dyn_cast<StructType>(dictEntryForVarSlot->getAllocatedType())` returned
// null (the cell holder is a `ptr*`, not a struct), so the key release was
// dead-coded away. The narrow fix-forward drops the `dictEntryForVarSlot`
// field entirely from this code path and reads the cell's struct type from
// `cellVars_[slot]` instead — `cellVars_` is populated on BOTH boxing
// paths in `boxLocalSlot` (the FRESH path that the inline non-escaping
// capture takes, and the GUARDED path; the previously considered
// `cellVarSyncSlot_` is only populated on the GUARDED path and would have
// left this case broken too).
//
// SCOPE. NOT extended to the for-loop-VARIABLE sibling (`for var entry in
// d` where `entry` is captured by a non-escaping lambda): `!isForVar` is a
// hard precondition of `boxedCellFree` itself (a for-var's cell is fresh
// per iteration; #4129/#4161's territory). NOT extended to
// `DrainedEntry<K,V>` (#4135, different lowering path with a different
// larger owned-field set). NOT extended to the escaping case (#4127,
// different release point: envdtor body rather than scope-end second
// pass).
//
// This is the parallel sibling of fix #4135 (DrainedEntry non-escaping
// capture Key+Value release) and fix #4127 (DictionaryEntry ESCAPING
// capture Key release).
//
// ORACLE. `runAotChecked` (ASan+LSan) loop-amplifies every leak-checking
// row 500x per LANE_RULES so a one-shot pass (which would pass on BOTH
// sides — the leaked allocation is still stack-reachable at process exit
// for a single call) cannot masquerade as a fix. `runAotCheckedLenient` is
// NOT used (per `silent-skip-buries-test-bugs.md` / #3654 — the lenient
// oracle would pass both pre- and post-fix vacuously and never pin the
// bug). FAIL-WITHOUT verification: with the src/ changes reverted, the
// amplified rows below FAIL with `AddressSanitizer: 17500 byte(s) leaked
// in 1000 allocation(s)`. PASS-WITH: 0 leaks.
//
// References: stdlib §4.8 (DictionaryEntry<K,V>), §4.9
// (DrainedEntry<K,V>), §27.5 (Closures), §28.9 (Runtime-managed types),
// v0.295.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// #3733 toolchain-SKIP burn-down. This file's 6 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Inline-program helper for the non-escaping capture: Compute declares fe
// bound to d.<terminal>, declares f = lambda() => fe.Key.Length + fe.Value,
// invokes f() once and returns. The lambda is non-escaping (a local var
// invoked and dropped) — the only path through which it reaches
// boxedCellFree in `consumeLastLambdaCapturedCells`.
std::string makeNonEscapingProgram(const std::string& declKw,
                                   const std::string& terminal) {
    return
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); " +
        declKw + " fe := d." + terminal + "; "
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
}

}  // namespace

// ---------------------------------------------------------------------------
// Value correctness (plain runAot — unaffected by this fix either way).
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim (single-shot, no amplification): `d.First()`
// over a two-entry dict inserted in order "a" then "xy" — First() is "a"
// (Key.Length=1) + Value=1 = 2.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           first_captured_non_escaping_closure_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var fe := d.First(); "
        "    var f := lambda() => fe.Key.Length + fe.Value; "
        "    return f(); "
        "  end Compute; "
        "  public static method Main(): int return C.Compute(); end Main; "
        "end C;"),
        2);
}

// ---------------------------------------------------------------------------
// Leak coverage (runAotChecked, 500x amplified) — the FAIL-WITHOUT /
// PASS-WITH axis this fix is actually about. `runAotChecked` enables
// ASan+LSan on the GENERATED Bezel code (per `silent-skip-buries-test-
// bugs.md` / `lsan-one-shot-leak-tests-pass-vacuously.md` — using
// `runAotCheckedLenient` here would let pre- and post-fix both PASS
// vacuously, never pinning the bug).
// ---------------------------------------------------------------------------

// First() row. FAIL-WITHOUT (HEAD without this fix): 17500 B / 1000
// allocations (35 B / 2 allocations per call, measured above).
// PASS-WITH (this fix): clean. "a"(1) + "xy"(9) inserted in that order;
// First() is "a" -> Key.Length=1 + Value=1 = 2, so total = 500*2 = 1000.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           var_first_captured_non_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeNonEscapingProgram("var", "First()"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// Last() row — a DIFFERENT terminal reaching the same
// isFreshDictEntryKeyReleaseTerminalLocal eligibility gate (First/Last/
// Single/ElementAt all rebuild the Key identically per #3720). Last() over
// {"a":1, "xy":9} is "xy" -> Key.Length=2 + Value=9 = 11, total =
// 500*11 = 5500.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           var_last_captured_non_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeNonEscapingProgram("var", "Last()"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}

// Single() row — exactly one element, exercising the same terminal
// eligibility gate with a single-entry dict. The program template above
// uses 2 entries; Single() throws on >1, so we hand-build a 1-entry dict:
// "a"(1) -> Key.Length=1 + Value=1 = 2, total = 500*2 = 1000.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           var_single_captured_non_escaping_closure_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
        "    var fe := d.Single(); "
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
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// ElementAt(0) row — a fourth terminal of the same isFreshDictEntryKeyRelease
// TerminalLocal gate (per #3720). First() == ElementAt(0) over {"a":1,
// "xy":9}: Key.Length=1 + Value=1 = 2, total = 500*2 = 1000.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           var_elementat_captured_non_escaping_closure_leak_free) {
    JitResult r = runAotChecked(makeNonEscapingProgram("var", "ElementAt(0)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// Two-non-escaping-closures row — two local lambdas both capturing the
// same `DictionaryEntry` local. `singleCaptureLocals_` is the safety case
// (#4165 v0): exactly one holder (one cell), exactly one +1 to balance.
// f() returns fe.Key.Length = 1; g() returns fe.Value = 1; total per call
// = 2; 500 calls = 1000.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           two_non_escaping_captures_same_local_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var fe := d.First(); "
        "    var f := lambda() => fe.Key.Length; "
        "    var g := lambda() => fe.Value; "
        "    return f() + g(); "
        "  end Compute; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Compute(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// Scope-edge disposal row — the cell-holder's slot lives in Compute()'s
// stack frame and is freed when Compute() returns. Without the fix the
// Key's calloc'd 32-byte header + 1-byte payload leak 33 B per call
// (Single() entry, "a" key). 500 calls → 500 × 33 = 16500 B direct + 500 × 1
// = 500 B indirect → 17000 B / 1000 allocs total. With the fix: 0 leaks.
// "a"(1) -> Key.Length=1 + Value=1 = 2; total = 500*2 = 1000.
BEZEL_TEST(dict_entry_non_escaping_string_key_4165,
           single_entry_dict_non_escaping_capture_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Compute(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
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
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}