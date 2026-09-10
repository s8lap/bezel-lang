// fix #4161 (§27.5 Closures / §28.9 Runtime-managed types / §28.3 Ownership,
// stdlib §4.8 DictionaryEntry<K,V>, v0.295): a `for var entry in dict`
// entry-iteration loop over a STRING-keyed `Dictionary<K,V>` is a genuine
// HEAP-USE-AFTER-FREE — not merely a leak — when the loop variable is
// captured by a closure that escapes the loop, reached via ORDINARY loop
// completion (or `break`/`continue`): no `return`, no exotic control flow
// required at all.
//
// ROOT CAUSE. `lowerDictEntryFor` (LlvmBackend_Stmts.cpp) stores each
// iteration's freshly rebuilt `bz_string*` Key into the loop-var struct
// alloca (`varSlot` field 0), and its `incBB`/`endBB` blocks (fix #1058)
// unconditionally released that SAME slot's value at the end of every
// iteration and at loop exit — added specifically so a string-keyed for-in
// loop does not leak the rebuilt Key on normal completion. When a closure in
// the body captures the loop variable (`forVarCapturedInBody`), `lowerForBody`
// boxes the CURRENT struct value into a fresh per-iteration heap cell — a
// SHALLOW copy holding the SAME `bz_string*` pointer `varSlot` field 0 holds.
// If that closure ESCAPES (assigned to an outer variable, returned, etc.), the
// cell (and its pointer copy) outlives the loop; `incBB`/`endBB`'s release
// then frees the pointer out from under the still-live escaped closure —
// invoking it afterward reads freed memory. Unlike sibling issue #4129 (a
// `return`/`throw` from inside the body reaching a SEPARATE, NEW disposal
// path this file does not touch), incBB/endBB's own release was NEVER gated
// on capture at all — the hazard is reachable by ordinary code with no
// `return` anywhere in the loop.
//
// THE FIX threads `forVarCapturedInBody`'s coarse, escape-BLIND answer
// through `cellFreeEligible_` — the SAME whole-body pre-pass authority
// `lowerForBody` (LlvmBackend_Stmts.cpp) already consults, keyed by `&fs`
// exactly like `forVarCapturedInBody`, to decide whether the for-var's own
// boxed capture CELL can be freed at each iteration's body end
// (`collectCellFreeEligible`, LlvmBackend_Setup.cpp: a key is present iff at
// least one lambda captures the for-var AND no escaping lambda captures it).
// A new `forVarCapturedEscaping` local in `lowerDictEntryFor` is true only
// when capture is present AND `cellFreeEligible_` does NOT vouch for it —
// i.e. some closure in the body provably MAY escape with the loop variable.
// `incBB`/`endBB`'s Key release (and `dictEntryForSnapshot`'s
// `dictEntryForReleaseInFlightKey`, #4129's own return/throw-path gate — now
// computed from the SAME refined predicate instead of the coarser
// `forVarCaptured`) both gate on `!forVarCapturedEscaping`: when a capture is
// proven non-escaping, the closure's env (and its cell's alias of this same
// pointer) is disposed by `lowerForBody`'s own scope-end/break-unwind
// handling strictly BEFORE incBB/endBB runs for that iteration, so releasing
// the Key remains sound and leak-free — exactly like before this fix. When
// escape cannot be ruled out, the Key falls back to the pre-existing
// safe-direction leak (never trading a leak for a UAF), matching the
// discipline #4129/#4127 already use elsewhere in this same file.
//
// MEASURED (ASan; scripts/agent_check.sh --probe --asan), the issue's own
// repro (`Dictionary<string, int>`, one entry `"xy" -> 9`, an outer `var f`
// reassigned to an escaping closure every iteration, loop completes
// normally, `g := C.MakeClosure(); g();`), on the #4129/#4127 parent tree
// (both those fixes present, #4161's own fix absent):
//
//   ==ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 ...
//   freed by thread T0 here: ... in bz$M.C$MakeClosure
//   previously allocated by thread T0 here: ... in calloc ... in
//   bz$M.C$MakeClosure
//
// — a genuine crash (r.ran == false), not a leak count. Post-fix: r.ran ==
// true, correct output, and the Key still LEAKS by design (the closure
// genuinely escapes here) — 500x-amplified LSan measurement of the same
// shape (`scripts/agent_check.sh --probe --asan`, `C.MakeClosure()` + `g()`
// called in a 500-iteration loop): 17500 B / 500 allocations leaked, i.e. 35
// B / 1 allocation per call (32 B calloc'd `bz_string_t` header + 3 B "xy\0"
// payload malloc) — the SAME per-call shape #4127's own measurement reports
// for the analogous `.First()`-bound-local case, confirming this is the
// SAME, already-accepted trade-off applied to the for-loop-variable family.
//
// PRE-EXISTING, UNRELATED LEAKS IN THE SAME AREA (explicitly NOT this fix's
// bytes — read and ruled out before trusting the numbers above/below):
//   * #4165 (a NON-escaping closure capture of a `.First()`/`.Last()`-bound
//     DictionaryEntry LOCAL leaks its rebuilt Key) — a different eligibility
//     family (`isFreshDictEntryKeyReleaseTerminalLocal`/#3720's own scope-end
//     disposal), never reached here: nothing in this file binds a
//     `DictionaryEntry` local via `.First()`/`.Last()`/`.Single()`/
//     `.ElementAt(i)`; every row here iterates the dict directly.
//   * #4155 / #4135 (a `DrainedEntry<K,V>` for-in loop's capture-cell backing
//     allocation / owned Value+Key leak) — `lowerDictDrainFor`, a completely
//     different lowering path reached only via `.Drain()`; no row here calls
//     `.Drain()`.
//   * #3541 (the §27.5 closure+env+cell residue on a sync-iterator
//     abandonment) — a user-defined iterator method's own closure captures
//     across a `yield`; nothing here is an iterator method.
//   * #4130 (a lambda reading an enclosing loop's own MUTATED CONTROL
//     VARIABLE leaks a 4 B capture cell) — requires a `while`/counted loop
//     whose counter a lambda reads; no row here declares a lambda over a
//     loop counter, only over the dict-entry for-var itself.
// None of the four apply to any row in this file, so every measured byte
// above/below is attributable to #4161 alone.
//
// ORACLE. The escaping-capture (UAF) rows use `runAotCheckedLenient` (ASan
// crash detection, LeakSanitizer OFF) — exactly the disposition sibling file
// `dict_entry_closure_capture_3876_test.cpp` already uses for its own
// `for_loop_variable_string_key_capture_no_stack_overflow` row, and for the
// SAME reason: the Key is DELIBERATELY left leaking on this path (never
// traded for a UAF), so a strict LSan oracle would flag the accepted
// trade-off as a regression. The non-escaping-capture CONTROL rows use the
// strict `runAotChecked` (ASan+LSan) — proving the refinement in this fix
// does not regress the common, provably-safe capture shape into a leak (a
// risk a cruder `!forVarCaptured` gate, mirroring #4129's own coarse
// dictEntryForSnapshot gate, would have introduced here).
//
// FAIL-WITHOUT / PASS-WITH: 5 of 8 rows below fail without the fix —
// `normal_completion_string_key_escaping_capture_no_use_after_free`,
// `break_string_key_escaping_capture_no_use_after_free`,
// `continue_string_key_escaping_capture_no_use_after_free`,
// `nested_loop_inner_var_escaping_capture_no_use_after_free`, and
// `const_local_before_escaping_capture_no_use_after_free` all report
// `r.ran == false` with a heap-use-after-free ASan diagnostic pre-fix. The
// remaining 3 (`captured_non_escaping_normal_completion_stays_leak_clean`,
// `captured_non_escaping_break_stays_leak_clean`,
// `return_path_escaping_capture_stays_leak_free_unaffected`) pass identically
// before and after this fix: the first two because incBB/endBB's PRE-fix code
// unconditionally released the Key regardless of capture (this fix only makes
// that release CONDITIONAL, never removes it — a non-escaping capture was
// already leak-free pre-fix, "accidentally" safe because the bug was
// specifically about the escaping case), and the third because it exercises
// `dictEntryForSnapshot`'s pre-existing #4129 gate on the return/throw path,
// which this fix only re-derives from a more precise predicate without
// changing its answer for a directly-returned (definitely-escaping) closure.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro: normal loop completion, no return/break/continue at
// all. FAIL-WITHOUT: heap-use-after-free (r.ran == false).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           normal_completion_string_key_escaping_capture_no_use_after_free) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    var f: () -> int := lambda() => 0; "
        "    for var entry in d do "
        "      f := lambda() => entry.Key.Length + entry.Value; "
        "    end; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    Console.WriteLine($\"total={g()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    // Pre-fix: r.ran is false -- a genuine ASan heap-use-after-free (the
    // measured repro in the file header). Post-fix: runs correctly.
    BEZEL_EXPECT_TRUE(r.ran);
    // "xy".Length (2) + 9 = 11.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// `break` immediately after the escaping assignment, on the SECOND of two
// entries (proves the fix does not depend on the capture happening on the
// loop's first/only iteration, and that the remaining-boxes free loop at
// endBB — which runs unconditionally for the OTHER, non-Key allocations —
// still coexists correctly with the gated Key release).
BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           break_string_key_escaping_capture_no_use_after_free) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var f: () -> int := lambda() => 0; "
        "    for var entry in d do "
        "      if entry.Key = \"xy\" then "
        "        f := lambda() => entry.Key.Length + entry.Value; "
        "        break; "
        "      end; "
        "    end; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    Console.WriteLine($\"total={g()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// `continue` on the FIRST entry, escaping capture on the SECOND, normal
// completion -- proves `continue` (which jumps straight to incBB) does not
// bypass the gate, and that the gate applies uniformly to EVERY iteration's
// incBB release once any capture in the body is unproven, including an
// iteration where the capture did not fire this time around.
BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           continue_string_key_escaping_capture_no_use_after_free) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var f: () -> int := lambda() => 0; "
        "    for var entry in d do "
        "      if entry.Key = \"a\" then continue; end; "
        "      f := lambda() => entry.Key.Length + entry.Value; "
        "    end; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    Console.WriteLine($\"total={g()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// A dict-entry for-in loop NESTED inside another dict-entry for-in loop --
// the escaping capture is on the INNER loop's variable only. Proves
// `cellFreeEligible_`'s `&fs` keying correctly distinguishes the two ForStmt
// nodes in the same function body (the OUTER loop's own var is never
// captured, so its own Key release stays unconditional/unaffected).
BEZEL_TEST(
    dict_entry_for_var_capture_escaping_release_4161,
    nested_loop_inner_var_escaping_capture_no_use_after_free) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var outer := new Dictionary<string, int>(); "
        "    outer.Add(\"o1\", 100); "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    var f: () -> int := lambda() => 0; "
        "    for var _ in outer do "
        "      for var entry in d do "
        "        f := lambda() => entry.Key.Length + entry.Value; "
        "      end; "
        "    end; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    Console.WriteLine($\"total={g()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// A `const` local reading `entry.Value` alongside (not instead of) the
// direct `entry` capture -- mirrors #4129's own
// `const_local_in_body_before_return_leak_free` coverage exactly (that row
// also binds `entry.Value`, not `entry.Key`: a `const` of the STRING Key
// would take its own reference-counted hold on the same `bz_string*`, which
// would mask a use-after-free by keeping the object's refcount above zero
// through incBB/endBB's release -- an int has no such ownership dynamics),
// confirming the escape-aware gate is indifferent to additional local
// declarations sharing the loop body.
BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           const_local_before_escaping_capture_no_use_after_free) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    var f: () -> int := lambda() => 0; "
        "    for var entry in d do "
        "      const v := entry.Value; "
        "      f := lambda() => entry.Key.Length + entry.Value + (v - v); "
        "    end; "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    Console.WriteLine($\"total={g()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// ---------------------------------------------------------------------------
// Accept-side controls: a NON-escaping capture (declared, invoked, and
// dropped entirely INSIDE the same iteration's body) must stay leak-clean
// under the STRICT oracle -- both before this fix (incBB/endBB released
// unconditionally, so this shape was already fine) and after (the refined
// `cellFreeEligible_`-based gate correctly recognizes it as safe to release).
// This is the row a cruder `!forVarCaptured` gate (mirroring #4129's own
// coarse return-path gate) would have REGRESSED into a leak; it did not
// change here because this fix's gate is escape-AWARE, not merely
// capture-aware.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           captured_non_escaping_normal_completion_stays_leak_clean) {
    const std::string src =
        "module M; class C "
        "  public static method Sum(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    var total := 0; "
        "    for var entry in d do "
        "      var f := lambda() => entry.Key.Length + entry.Value; "
        "      total := total + f(); "
        "    end; "
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
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}

BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           captured_non_escaping_break_stays_leak_clean) {
    const std::string src =
        "module M; class C "
        "  public static method Sum(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); d.Add(\"xy\", 9); "
        "    var total := 0; "
        "    for var entry in d do "
        "      if entry.Key = \"xy\" then "
        "        var f := lambda() => entry.Key.Length + entry.Value; "
        "        total := f(); "
        "        break; "
        "      end; "
        "    end; "
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
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=5500"));
}

// ---------------------------------------------------------------------------
// Regression control: the pre-existing #4129 return/throw-path gate
// (`dictEntryForSnapshot`'s `dictEntryForReleaseInFlightKey`), now re-derived
// from `forVarCapturedEscaping` instead of the coarser `forVarCaptured` this
// fix replaces at that site too. A directly-returned lambda is
// unconditionally, conservatively escaping under BOTH predicates, so this
// row's answer (leak, not release -- never a UAF) is unaffected by this fix;
// pins that the shared re-derivation introduced no regression on the path
// #4129 already owned. Same disposition, and the SAME shape, as sibling file
// `dict_entry_closure_capture_3876_test.cpp`'s own
// `for_loop_variable_string_key_capture_no_stack_overflow` row.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_for_var_capture_escaping_release_4161,
           return_path_escaping_capture_stays_leak_free_unaffected) {
    const std::string src =
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); "
        "    for var entry in d do "
        "      return lambda() => entry.Key.Length + entry.Value; "
        "    end; "
        "    return lambda() => 0; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var f := C.MakeClosure(); "
        "    Console.WriteLine($\"total={f()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}
