// Codegen — fix #4129 (§28.3 / §29.12, stdlib §4.8, v0.295): a `for var entry
// in dict` entry-iteration loop (`lowerDictEntryFor`, LlvmBackend_Stmts.cpp)
// leaks its own key/value SNAPSHOT SPINE arrays — the malloc'd arrays
// bz_dict_keys_str/bz_dict_keys_gen/bz_dict_values_str/bz_dict_values_gen
// hand back before the loop starts — on a `return` from inside the loop
// body. §29.12 requires the runtime to detect concurrent mutation across the
// whole enumeration; the compiler-internal bookkeeping that makes that
// possible (this snapshot) is not owned by user code and so is the
// compiler's own obligation to reclaim exactly once (§28.3's "every heap
// object disposed exactly once" applies to compiler-introduced allocations
// the same as user ones). Reproduces with NO closure, capture, or string key
// involved — a plain `return entry.Value;` from inside the loop body is
// enough — which makes this a distinct, more general defect than #3876/#4127
// (both scoped to a captured/escaping `DictionaryEntry` local specifically).
//
// ROOT CAUSE. `lowerDictEntryFor` snapshots the dict's key/value tables
// before the loop, and its OWN manual "endBB" code frees them — but only on
// the two edges that actually branch to endBB: normal completion (the
// condition going false) and `break` (LlvmBackend_Stmts.cpp's loopStack_-
// driven jump). A `return` executed anywhere inside the loop body emits an
// LLVM `ret` directly and never reaches endBB, so the snapshot arrays, and
// — for a STRING key — the in-flight iteration's freshly rebuilt
// `bz_string*` Key (decodeDictStringKeyAt's `strObj`) plus that key's
// snapshot box, all leak.
//
// MEASURED (500x amplified under ASan/LSan; scripts/agent_check.sh --probe
// --asan), on the parent commit (2aa5b1d), the issue's own repro
// (`Dictionary<string, int>`, one entry, `return entry.Value;` on the first
// and only iteration):
//
//   total=4500
//   SUMMARY: AddressSanitizer: 31000 byte(s) leaked in 3000 allocation(s).
//
// — exactly 62 B / 6 allocations per call: 32 B (calloc, the rebuilt
// `bz_string_t` header) + 8 B (bz_dict_keys_str's keys-array malloc) + 8 B
// (bz_dict_values_str's values-array malloc) + 8 B (bz_dict_keys_str's
// lens-array malloc) + 3 B (indirect, the rebuilt key's "xy\0" payload
// malloc) + 3 B (indirect, bz_dict_keys_str's own per-key strndup'd copy).
// The program's own value (total=4500 = 500 * 9) is correct on both sides —
// this is a pure leak, never a UAF or double-free.
//
// THE FIX. Register the snapshot (keysOutSlot/lensOutSlot/valsOutSlot/
// cntSlot/idxSlot, plus — for a string key — the loop-var struct's field 0)
// as its own disposeStack_ scope (a new `dictEntryForSnapshot` Disposal kind,
// LlvmBackend_impl.hpp / LlvmBackend_Disposal.cpp), pushed right after the
// snapshot exists and BEFORE loopStack_'s own entry (so `break`/`continue` —
// whose emitLoopDisposals only unwinds scopes AT OR ABOVE the loop's
// recorded depth — never see it, and keep using the pre-existing manual
// endBB code exclusively, avoiding a double free there) and popped WITHOUT
// emission once that manual endBB code has run. A `return` from inside the
// body reaches this new scope via the ordinary `emitAllDisposals()` /
// `emitDisposalsInRange()` walk any return statement already performs for
// every OTHER in-scope disposal, so it now reclaims the snapshot exactly
// once, from any nesting depth inside the loop body.
//
// PRE-EXISTING, UNRELATED LEAKS IN THE SAME AREA (explicitly NOT this fix's
// bytes — read and ruled out before trusting the numbers below):
//   * #4135 (a DrainedEntry-typed for-in loop leaking its owned Value/Key on
//     an escaping closure capture) — a different synthetic type
//     (`lowerDictDrainFor`), never reached by any row here (no `.Drain()`).
//   * #3541 (the §27.5 closure+env+cell residue on a sync-iterator
//     abandonment) — applies to a user-defined iterator method's own
//     closure captures across a `yield`; nothing here is an iterator method
//     or captures anything.
//   * #4130 (a lambda reading an enclosing loop's mutated control variable
//     leaks a 4 B capture cell) — requires a lambda declared inside an outer
//     loop reading that loop's own counter; no row here declares a lambda.
// None of the three apply to any row in this file (no closures, no
// `.Drain()`, no lambda anywhere in these programs), so every measured byte
// below is attributable to #4129 alone.
//
// ORACLE. `runAotChecked` (ASan/LSan on the generated program) loop-amplifies
// every leak-checking row 500x per LANE_RULES so a one-shot pass (which would
// pass on BOTH sides — the leaked allocation is still stack-reachable at
// process exit for a single call) cannot masquerade as a fix. Plain-`runAot`
// rows check only the returned VALUE (the correctness axis, unaffected by
// this fix either way).

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
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value correctness (plain runAot — unaffected by this fix either way; pins
// the return value so a future regression in the decode itself is caught by
// this same file).
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim: single string-keyed entry, `return
// entry.Value;` on the loop's first (only) iteration.
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129, string_key_return_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    for var entry in d do return entry.Value; end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int return C.Find(); end Main; "
        "end C;"),
        9);
}

// Generic (non-string) key: the bz_dict_keys_gen/bz_dict_values_gen arm.
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129, generic_key_return_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(7, 9); "
        "    for var entry in d do return entry.Value; end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int return C.Find(); end Main; "
        "end C;"),
        9);
}

// ---------------------------------------------------------------------------
// Leak coverage (runAotChecked, 500x amplified) — the FAIL-WITHOUT / PASS-
// WITH axis this fix is actually about.
// ---------------------------------------------------------------------------

// #4129 core repro, 500x: string key, `return` on the loop's first and only
// iteration. FAIL-WITHOUT: 31000 B / 3000 allocations (62 B / 6 allocations
// per call, measured above). PASS-WITH: clean.
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129,
           string_key_return_first_iteration_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    for var entry in d do return entry.Value; end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Find(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4500"));
}

// Generic (non-string) key twin of the row above — the bz_dict_keys_gen /
// bz_dict_values_gen arm's own per-key snapshot box leaks identically
// pre-fix (measured: clean pre-fix count was NOT re-derived per-row here,
// but the same `dictEntryForUseGen` branch this fix adds is exercised by
// this row and by nothing else in this file).
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129,
           generic_key_return_first_iteration_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(7, 9); "
        "    for var entry in d do return entry.Value; end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Find(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4500"));
}

// `return` from a NESTED `if` on the THIRD iteration (idx == 2 when the
// return fires, cnt == 3) — exercises the "free the remaining un-freed
// per-key snapshot boxes at [idx, cnt)" branch with a genuinely non-trivial
// range (idx cnt-1, not idx == 0 as in the two rows above, where [idx, cnt)
// is a single element). Also proves the fix does not depend on the return
// being lexically the loop body's only statement.
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129,
           string_key_return_from_nested_if_mid_iteration_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"b\", 2); d.Add(\"c\", 3); "
        "    for var entry in d do "
        "      if entry.Key = \"c\" then return entry.Value; end; "
        "    end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Find(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1500"));
}

// `const` binding of a VALUE read out of the entry before returning it — a
// `const` binding of the DICTIONARY itself is not a legal program to compare
// against (§21.1/§28.3: "a `const` local must not take an owned production" —
// `new Dictionary<...>()` is one, so `const d := new Dictionary<...>();` is a
// compile error independent of this fix, confirmed by probe). This row
// instead confirms the fix's new disposeStack_ scope — which is registered
// purely from the for-loop's own snapshot allocas, never from the loop
// body's own local declarations — is indifferent to whether the body binds
// its own locals `var` or `const` on the way to the `return`.
BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129,
           const_local_in_body_before_return_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Find(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    for var entry in d do "
        "      const v := entry.Value; "
        "      return v; "
        "    end; "
        "    return -1; "
        "  end Find; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Find(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=4500"));
}

// ---------------------------------------------------------------------------
// Accept-side controls: normal completion and `break` — both already used
// the pre-existing manual endBB code, and this fix's new disposeStack_ scope
// is deliberately positioned (pushed before loopStack_'s own entry) so
// neither edge ever sees it. These two rows prove that positioning holds —
// i.e., that this fix introduces no double-free on the two paths that were
// already leak-free. Both PASS identically before and after this fix
// (accept-side controls, not new coverage of the bug itself).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129,
           normal_completion_stays_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method Sum(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"b\", 2); d.Add(\"c\", 3); "
        "    var total := 0; "
        "    for var entry in d do total := total + entry.Value; end; "
        "    return total; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.Sum(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=3000"));
}

BEZEL_TEST(dict_entry_for_return_snapshot_leak_4129, break_stays_leak_free) {
    const std::string src =
        "module M; class C "
        "  public static method FindBreak(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"b\", 2); d.Add(\"c\", 3); "
        "    var found := -1; "
        "    for var entry in d do "
        "      if entry.Key = \"b\" then found := entry.Value; break; end; "
        "    end; "
        "    return found; "
        "  end FindBreak; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 500 do total := total + C.FindBreak(); i := i + 1; end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}
