// fix #3876 (§27.5 Closures / §28.9 Runtime-managed types / stdlib §4.8
// DictionaryEntry<K,V>, v0.295): a DictionaryEntry<K,V> local captured by a
// closure that is RETURNED from its producing method (escaping the method's
// own scope) and invoked afterward reads back garbage for a non-string key,
// and hits a genuine ASan stack-buffer-overflow for a string key.
//
// ROOT CAUSE (measured, NOT the issue's own filed hypothesis -- see below).
// The DictionaryEntry/DrainedEntry `.Key`/`.Value` MemberAccessExpr intercept
// (LlvmBackend_Exprs.cpp, the "#548 / stdlib §4.8" arm) resolves the target's
// storage with `localSlot(*ma->target)` and GEPs directly into whatever
// alloca that returns, assuming it IS the {K,V} struct's own storage. That
// holds for an ORDINARY (non-captured) local, whose `locals_` entry is the
// {K,V}-typed alloca `var fe := d.First();` itself created. But §27.5
// capture boxing (`Emitter::boxLocalSlot`, LlvmBackend_Layout.cpp; the
// env-restore loop, LlvmBackend_Exprs2.cpp ~line 2392) REBINDS `locals_[decl]`
// to a DIFFERENT, ptr-SIZED alloca that holds the shared heap CELL's
// address -- the actual {K,V} struct lives in that heap cell, not in the
// rebound alloca. GEP'ing directly into the rebound alloca:
//   - DictionaryEntry<int,int> = {i32,i32} (8 B), the SAME size as the
//     ptr-holder (8 B) -> in-bounds but WRONG: reads the low/high halves of
//     the CELL POINTER VALUE as if they were Key/Value. Silent garbage, no
//     ASan complaint -- matches the issue's own measurement ("clean under
//     ASan, a different garbage number each run").
//   - DictionaryEntry<string,int> = {ptr,i32} (16 B, padded), WIDER than the
//     8-byte ptr-holder -> the `.Value` field GEP (byte offset 8) reads 4
//     bytes past the end of the 8-byte stack object: a genuine
//     AddressSanitizer stack-buffer-overflow -- matches the issue's report
//     exactly ("READ of size 4 ... frame has 1 object [32,40) ... offset 40
//     overflows this variable": the object IS the 8-byte ptr-holder, and
//     offset 40 is 8 bytes past its start -- relative offset 8, the Value
//     field of a {ptr,i32} struct).
//
// THE FILER'S HYPOTHESIS ("computes the captured slot's size/layout from
// something that requires a real TypeSymbol/declaration") DID NOT HOLD. The
// capture-boxing SIZE computation (`boxLocalSlot`'s `slot->getAllocatedType()`
// feeding the calloc'd cell size) is correct regardless of TypeSymbol/
// declaration -- it sizes the heap cell from the ORIGINAL alloca's LLVM type,
// which is always right. The defect is entirely on the READ side: the
// DictionaryEntry/DrainedEntry intercept never checked whether the local it
// reads had been boxed for capture, so it never dereferenced the cell holder
// to reach the actual struct. `Emitter::boxedStorage` (LlvmBackend_Layout.cpp)
// is the existing §27.5 funnel every OTHER boxed local read/write in this
// codebase already goes through for exactly this reason; this one intercept
// was simply never routed through it. The issue's own control (`Pt`, a REAL
// declared record) never hits this intercept at all -- an ordinary record
// reaches the general `fieldAddr` path, which already calls `boxedStorage` --
// which is exactly why the control worked pre-fix and only the synthetic
// DictionaryEntry/DrainedEntry types were affected.
//
// THE FIX: route both the DictionaryEntry and DrainedEntry `.Key`/`.Value`
// intercepts (LlvmBackend_Exprs.cpp) through `boxedStorage(slot, elemTy)`
// instead of GEP'ing `slot` directly. For an ordinary, non-captured local,
// `cellVars_.find(slot)` misses inside `boxedStorage` and it returns `slot`
// unchanged (byte-for-byte the pre-fix behaviour) -- so the working,
// non-escaping shape already covered by
// `dict_first_last_elementat_3576_test.cpp` is unaffected.
//
// FAIL-WITHOUT / PASS-WITH, measured on this file against HEAD 102d29f
// (scratch revert of the two LlvmBackend_Exprs.cpp arms + relink, then
// restore + relink): 4 of 4 non-control rows below fail without the fix --
// `non_string_key_var_captured_across_return_reads_correct_value` and its
// `const` sibling read a wrong (garbage, ASLR-dependent) value pre-fix, and
// `string_key_var_captured_across_return_no_stack_overflow` and its `const`
// sibling report a genuine ASan stack-buffer-overflow pre-fix (r.ran=false).
// The two control rows (`control_record_capture_unaffected`,
// `for_loop_variable_string_key_capture_no_stack_overflow`) pass on BOTH
// sides pre- and post-fix in one respect (the record control was never
// broken -- see root cause above) and, for the for-loop-variable row, is a
// second, independently-reachable instance of the SAME defect this fix
// closes (not a distinct row the FAIL-WITHOUT count above claims).
//
// A SEPARATE, PRE-EXISTING, formerly-undischarged FINDING, NOW FIXED by
// #4127 (this file's two plain-`.First()`-bound-local string-key rows below
// were tightened from `runAotCheckedLenient` to `runAotChecked` in that
// commit): the string-key rows used to leak the rebuilt `bz_string*` Key
// (32+3 B, measured) under the strict `runAotChecked` (ASan+LSan) oracle.
// This was fix #3720's OWN documented trade-off ("An ESCAPING local
// (returned, stored, captured, reassigned, passed elsewhere) is simply not
// registered [for the scope-end release], so its rebuilt Key leaks ... never
// a UAF, never a double-free -- only ever a leak"):
// `isFreshDictEntryKeyReleaseTerminalLocal`'s eligibility walk does not (and,
// per that fix's own stated scope, was never meant to) admit a local that
// escapes via closure capture -- #3876 did not touch that eligibility logic,
// so the leak was unchanged by it. #4127 closes it from the OTHER side: the
// per-lambda envdtor (LlvmBackend_Exprs2.cpp's `lowerLambda`) now recognizes
// a captured `DictionaryEntry<string,V>` local's own §694 `freeCellInEnvdtor`
// eligibility (exactly one capturing closure, which definitely escapes) and
// releases the Key there too, immediately before freeing the cell itself --
// the single-holder/definite-escape proof that already makes the cell-free
// sound applies identically to the Key it contains (`isDictEntryStringKeyTypeRef`,
// LlvmBackend_impl.hpp). Deliberately narrower than the for-loop-VARIABLE
// sibling below (`for_loop_variable_string_key_capture_no_stack_overflow`):
// a for-var's cell is fresh per iteration, so `!isForVar` is a hard
// precondition of `freeCellInEnvdtor` itself -- that shape stays on
// `runAotCheckedLenient` (still leaks the Key; #4129/#4161 territory,
// unaffected by #4127).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

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

// `declKw` is "var" or "const" -- both bind through the identical §21.1
// LocalSymbol machinery `localSlot`/`boxedStorage` key on, so both flavors
// exercise the same fixed code path.
std::string nonStringKeyProgram(const std::string& declKw) {
    return
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<int, int>(); "
        "    d.Add(3, 9); " +
        declKw + " fe := d.First(); "
        "    return lambda() => fe.Key + fe.Value; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var f := MakeClosure(); "
        "    Console.WriteLine($\"total={f()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

std::string stringKeyProgram(const std::string& declKw) {
    return
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"xy\", 9); " +
        declKw + " fe := d.First(); "
        "    return lambda() => fe.Key.Length + fe.Value; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var f := MakeClosure(); "
        "    Console.WriteLine($\"total={f()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Non-string key: the issue's Repro 1 (garbage value, ASan-clean).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_closure_capture_3876,
           non_string_key_var_captured_across_return_reads_correct_value) {
    JitResult r = runAotChecked(nonStringKeyProgram("var"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 3 (Key) + 9 (Value) = 12. Pre-fix: garbage (the ptr-holder's raw bits).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=12"));
}

BEZEL_TEST(dict_entry_closure_capture_3876,
           non_string_key_const_captured_across_return_reads_correct_value) {
    JitResult r = runAotChecked(nonStringKeyProgram("const"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=12"));
}

// ---------------------------------------------------------------------------
// String key: the issue's Repro 2 (genuine ASan stack-buffer-overflow).
// `runAotChecked` (STRICT: ASan+LSan) since fix #4127 -- see the file
// header's note. Pre-#4127 these two rows used `runAotCheckedLenient`
// (LeakSanitizer off) to route around the then-undischarged rebuilt Key.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_entry_closure_capture_3876,
           string_key_var_captured_across_return_no_stack_overflow) {
    JitResult r = runAotChecked(stringKeyProgram("var"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-#3876: r.ran is false here -- a genuine AddressSanitizer
    // stack-buffer-overflow report in `diagnostics`, exactly the issue's
    // measured crash. Pre-#4127 (post-#3876): r.ran is true but LeakSanitizer
    // reports 35 B / 2 allocations (the rebuilt Key's calloc'd header +
    // malloc'd payload) -- this row would have failed under the strict
    // oracle used here now.
    BEZEL_EXPECT_TRUE(r.ran);
    // 2 (Key.Length) + 9 (Value) = 11.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

BEZEL_TEST(dict_entry_closure_capture_3876,
           string_key_const_captured_across_return_no_stack_overflow) {
    JitResult r = runAotChecked(stringKeyProgram("const"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// ---------------------------------------------------------------------------
// Controls.
// ---------------------------------------------------------------------------

// The issue's own control: an ORDINARY (non-synthetic) record with the same
// two-field shape, captured and returned the identical way. This never hit
// the buggy intercept at all (records go through the general `fieldAddr` /
// `boxedStorage` path, which was always correct) -- pins that this fix does
// not perturb the already-working path. Leak-clean (`runAotChecked`): a
// captured `string` field IS in this fix's own eligible-disposal family
// (ordinary field release, not the one-off #3720 special case), so unlike
// the DictionaryEntry rows above this one has no adjacent pre-existing leak
// to route around.
BEZEL_TEST(dict_entry_closure_capture_3876,
           control_record_capture_unaffected) {
    const std::string src =
        "module M; public record Pt(Name: string, N: int); "
        "class C "
        "  public static method MakeClosure(): () -> int "
        "    var p := new Pt(\"xy\", 9); "
        "    return lambda() => p.Name.Length + p.N; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var f := MakeClosure(); "
        "    Console.WriteLine($\"total={f()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}

// A second, independently-reachable instance of the SAME defect: the
// DictionaryEntry-typed FOR-LOOP VARIABLE (`for var entry in dict do`) goes
// through the identical `boxLocalSlot`/env-restore/MemberAccessExpr-intercept
// path as the plain `.First()`-bound local above (the for-loop-variable
// capture case uses `forLoopVar`-flavored boxing, but the READ side this fix
// changes is shared code, untouched by that flag). `runAotCheckedLenient`
// for the same reason as the string-key rows above: measured, this shape
// leaks MORE than the plain-local rows (78 B / 7 allocations here, vs. 35 B
// / 2 allocations there) -- the for-in dict enumerator's own key/value
// snapshot spine arrays are ALSO never freed when the loop is exited via a
// `return` from inside the body, on top of the #3720-gap Key leak. Both are
// pre-existing, unrelated to this fix. The Key leak is filed as #4127; the
// spine-array leak reproduces even with NO closure/capture at all (a plain
// `return entry.Value;` from inside the loop leaks it identically) and is
// filed separately as #4129.
BEZEL_TEST(dict_entry_closure_capture_3876,
           for_loop_variable_string_key_capture_no_stack_overflow) {
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
        "    var f := MakeClosure(); "
        "    Console.WriteLine($\"total={f()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=11"));
}
