// fix #4136 (§27.5 Closures / §28.2 / §28.3 / stdlib §4.9 DrainedEntry<K,V>,
// v0.295): the explicit `entry.Dispose()` codegen arm for a `DrainedEntry<K,V>`
// loop variable (`for var entry in d.Drain() do ... end`) GEPs directly into
// `localSlot(*ma->target)` instead of routing through `Emitter::boxedStorage` --
// the SAME defect class fix #3876 closed for the `.Key`/`.Value` READ
// intercept, one call site over in `LlvmBackend_Calls2.cpp` instead of
// `LlvmBackend_Exprs.cpp`. Filed as issue #4136 by the #3876 reviewer, who
// swept all `localSlot()` call sites in `src/codegen/` for the same shape and
// found this one live instance.
//
// ROOT CAUSE (mechanism identical to #3876; see that fix's own comment in
// LlvmBackend_Exprs.cpp for the full explanation). `entry`'s ordinary local
// storage is a `{K key; V* value}` struct alloca. When `entry` is captured by
// a closure (a lambda literal anywhere in the enclosing scope references it --
// §27.5 capture boxing applies to the ENTIRE lexical lifetime of the local in
// that scope, independent of invocation order), `locals_[entry]`'s
// `localSlot()` alloca is REBOUND to a different, pointer-sized cell-HOLDER
// alloca that holds the address of a shared heap cell -- the real `{K, V*}`
// struct lives in that heap cell, not in the holder. The explicit-`Dispose()`
// arm (`src/codegen/LlvmBackend_Calls2.cpp`, the `#2457` / stdlib §4.9 /
// §28.2 / §28.3 block) GEPs `CreateStructGEP(entStTy, slot, ...)` directly
// into the (possibly rebound) `slot` at field 1 (`Value`, "de.val.ptr") and
// field 0 (`Key`, "de.key.ptr"), assuming it IS the struct. `DrainedEntry<K,V>`
// requires `V: class` (stdlib §4.9), so `V` always lowers to `ptr` --
// `entStTy` is therefore ALWAYS at least 16 bytes (K padded to the 8-byte
// alignment `V`'s pointer field needs, then the 8-byte pointer itself), never
// as small as the 8-byte pointer-holder. **There is consequently no reachable
// "same size, wrong value" (8-byte) shape at THIS call site** -- unlike
// #3876's DictionaryEntry `.Key`/`.Value` read intercept, where `V` could be
// a non-owned value type (`DictionaryEntry<int,int>` = `{i32,i32}` = 8 bytes,
// exactly the holder's size). Every reachable shape here is a genuine
// AddressSanitizer stack-buffer-overflow: the `.Value` GEP (field 1, byte
// offset 8) reads 8 bytes starting at offset 8 of an 8-byte object, exactly
// the `[32, 40) ... offset 40 overflows this variable` signature #4136's own
// filed report and #3876's report both carry (measured identically below for
// both a K=int and a K=string entry).
//
// THE FIX: route both GEPs (the `.Value` disposal GEP and the `.Key`-release
// GEP, guarded on K being `string`) through `boxedStorage(slot, elemTy)`
// instead of GEP'ing `slot` directly -- exactly fix #3876's remedy, applied to
// the sibling call site. For a non-captured `entry` (no lambda anywhere
// references it), `cellVars_.find(slot)` misses inside `boxedStorage` and it
// returns `slot` unchanged, so the already-working non-captured path
// (`owned_dict_drain_628_test.cpp` et al.) is unaffected -- pinned below by
// `control_uncaptured_loop_var_explicit_dispose_unaffected`, which never hits
// the boxed branch at all and is deliberately run under the STRICT
// `runAotChecked` (ASan+LSan) oracle, unlike the two capturing rows below.
//
// `var`/`const` coverage: only `var` is constructible for a `for` loop
// variable -- language spec §21.4's grammar has no `for const` spelling (only
// `for var x in ... do ... end`, confirmed against the grammar and every
// worked example in that section) -- so there is no `const` sibling to add
// here, unlike a plain local declaration.
//
// FAIL-WITHOUT / PASS-WITH, measured on this file's two non-control programs
// against base 2aa5b1d (scratch revert of the `boxedStorage` routing in
// `LlvmBackend_Calls2.cpp` + relink, then restore + relink -- LANE_RULES §3):
// 2 of 2 non-control rows fail without the fix --
// `int_key_captured_loop_var_explicit_dispose_no_stack_overflow` and
// `string_key_captured_loop_var_explicit_dispose_no_stack_overflow` both hit a
// genuine AddressSanitizer stack-buffer-overflow pre-fix (r.ran=false under
// `runAotCheckedLenient`), reporting the identical
// `[32, 40) ... offset 40 overflows this variable` frame layout in both cases
// (see /var/tmp/bezel-scratch/r17/4136/logs/l1-4136-pre-probe3.log for K=int,
// l1-4136-pre-probe2.log for K=string) -- and both run clean with the correct
// value post-fix (see l1-4136-probe3.log / l1-4136-probe2.log in the same
// directory). The control row passes on BOTH sides for the stated reason
// above (never reaches the boxed branch).
//
// A SEPARATE, PRE-EXISTING, NOT-FIXED-HERE FINDING: with the fix applied, both
// capturing rows below leak their `entry` capture cell's own backing
// allocation (16 bytes for the K=int row: exactly one `{i32(padded), ptr}`
// heap cell, confirmed by LSan reporting a SINGLE 16-byte direct leak with no
// second entry for the `Widget` -- i.e. this fix's own `Value` disposal GEP
// correctly reaches and frees the real `Widget`, and only the cell HOLDER
// itself is never reclaimed; 64 bytes / 3 allocations for the K=string row,
// which additionally carries the drained string key's own backing bytes).
// This is a narrower defect than #4135 (which never calls `Dispose()`
// explicitly at all and therefore leaks the `Value` object itself, not just
// the cell) -- filed separately as #4155 for this cell-holder-never-freed gap,
// not fixed here (out of scope for a crash fix). Both rows below therefore use
// `runAotCheckedLenient` (generated-code ASan active; LeakSanitizer off),
// exactly mirroring #3876's own precedent for its adjacent, already-known
// leaks (#4127, #4129).
//
// ALSO FOUND WHILE MEASURING (NOT fixed here, filed separately as #4156): the
// filed issue's own literal repro -- which additionally RETURNS the closure
// and INVOKES it after `entry.Dispose()`, reading `entry.Key.Length` from
// inside the invoked closure -- still SEGVs on the fixed tree (a NULL-pointer
// read inside the lambda body, not a stack-buffer-overflow: `Dispose()`
// correctly nulls the released `Key` field via this very fix's corrected GEP,
// and the non-nullable `string` field read then dereferences that null with
// no null-guard). This is a distinct, deeper soundness question (whether
// §28.3 should permit a closure to observe a captured binding's post-Dispose
// state at all) that this narrowly-scoped crash fix does not attempt to
// answer; the two programs below deliberately avoid invoking a closure that
// reads a field `Dispose()` has released, so as to isolate the
// stack-buffer-overflow this fix actually targets.
//
// UPDATE (R17 fix-forward): #4156 was subsequently FIXED, later in this same
// batch, by a DIFFERENT mechanism than the above paragraph anticipated --
// not permitting the post-Dispose read at all, but rejecting at COMPILE TIME
// any closure that captures an already-explicitly-disposed local and then
// escapes the method via `return` (§28.2/§28.9). That correctly caught the
// K=int row's program below too (it also returned its capturing closure),
// which forced a rewrite -- see the comment immediately above
// `intKeyProgram()`.

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

// R17 fix-forward (narrow, test-only; gate cycle 3 went RED on exactly this
// row). The program below was REWRITTEN because fix #4156
// (`checkIndirectEscapeOnReturn`'s new Disposed-source arm, §28.2/§28.9,
// v0.294) landed LATER in this same 26-commit batch than this file, and its
// new diagnostic correctly rejects the ORIGINAL program that used to live
// here: `entry` was captured by a lambda `f`, `entry.Dispose()` ran, and `f`
// was then `return`ed from `MakeClosure` (escaping the method), relying on
// `Main()` invoking the returned closure afterward to read `entry.Key` -- a
// field #4136's fix proved is never touched by `Dispose()` for a K=int
// entry, so that specific runtime read was safe.
//
// But the ORIGINAL program was unsound independent of Dispose timing or
// which field the closure reads: per spec §28.3 "Local variables,
// ownership, and borrows", the "Captured owned local inside a closure"
// producers row (docs/Bezel_Language_Spec.md) states the capture is
// "Borrowed (source: the local's enclosing scope) ... the closure cannot
// outlive the local's scope", and the borrow-lifetime-rules bullet on
// closures repeats it: a closure "may not be ... returned from the
// enclosing method ... while its source set is non-empty". `entry`'s scope
// is the `for`-loop's per-iteration body -- strictly narrower than
// `MakeClosure` -- so a closure capturing it may never be returned from
// `MakeClosure`, disposed or not. The Owned-state instance of exactly this
// shape was ALREADY a compile error before this batch
// (`checkClosureEscapesOnReturn`'s `st.state == Owned` arm); fix #4156
// closes the SAME rule's Disposed-state gap, which is the ONLY reason the
// original program here used to compile at all -- it was pinning a missed
// rejection (a compiler bug), not a legitimate accept case (LANE_RULES
// §15 case (a): "the row pinned unsound behaviour and must change").
//
// Rewritten to invoke `f()` and consume its value BEFORE `entry.Dispose()`,
// entirely inside the loop body -- the closure is never returned and never
// outlives `entry`'s scope, so fix #4156 does not apply. `entry` is still
// captured by a lambda literal appearing in its scope, which per the file
// header above forces §27.5 capture boxing of its slot for the local's
// WHOLE lexical lifetime independent of invocation order, so
// `entry.Dispose()` below still resolves through the boxed cell and still
// exercises fix #4136's `boxedStorage`-routed disposal GEP -- the actual
// defect class this file exists to pin.
//
// Measured on this worktree's HEAD (b802527): the pre-rewrite program FAILS
// to compile with fix #4156's new diagnostic ("... is a closure capturing
// `entry`, which was explicitly disposed before this return ..."); this
// rewritten program PASSES (r.ran=true, stdout contains "key=7"). Logs:
// /var/tmp/bezel-scratch/r17/ff-4136/logs/l1-4136-probe-pre-rewrite.log
// (rejection) and .../l1-4136-probe-post-rewrite.log (key=7, clean run).
// The sibling `string_key_captured_loop_var_explicit_dispose_no_stack_overflow`
// row below needed NO change: its lambda is assigned to the discard `_` and
// never returned, so it never reaches #4156's return-escape check.
//
// K=int: DrainedEntry<int, Widget> = {i32(padded to 8), ptr} = 16 bytes.
std::string intKeyProgram() {
    return
        "module M; public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method MakeValue(): int "
        "    using var d := new OwningDictionary<int, Widget>(); "
        "    d.Add(7, new Widget(9)); "
        "    for var entry in d.Drain() do "
        "      var f := lambda() => entry.Key; "
        "      var k := f(); "
        "      entry.Dispose(); "
        "      return k; "
        "    end; "
        "    return -1; "
        "  end MakeValue; "
        "  public static method Main(): int "
        "    var k := MakeValue(); "
        "    Console.WriteLine($\"key={k}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// K=string: DrainedEntry<string, Widget> = {ptr, ptr} = 16 bytes -- exercises
// BOTH fixed GEPs (the `.Value` disposal GEP, Step 1, and the string-`Key`
// release GEP, Step 2), unlike the K=int program above which only exercises
// Step 1 (Step 2 is a no-op for a non-string key). The value under test
// (`entry.Value.N`) is read BEFORE the explicit `Dispose()` call, and the
// (necessarily capturing, to force boxing) closure is discarded, never
// invoked -- see the file header's note on why this program does not invoke a
// closure that reads `entry.Key` post-Dispose.
std::string stringKeyProgram() {
    return
        "module M; public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method MakeClosure(): int "
        "    using var d := new OwningDictionary<string, Widget>(); "
        "    d.Add(\"xy\", new Widget(9)); "
        "    var result := 0; "
        "    for var entry in d.Drain() do "
        "      _ := lambda() => entry.Key.Length; "
        "      result := entry.Value.N; "
        "      entry.Dispose(); "
        "    end; "
        "    return result; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var r := MakeClosure(); "
        "    Console.WriteLine($\"result={r}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// The two rows that fail without the fix (2 of 2 -- see file header).
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_drain_entry_dispose_capture_4136,
           int_key_captured_loop_var_explicit_dispose_no_stack_overflow) {
    JitResult r = runAotCheckedLenient(intKeyProgram());
    if (wasSkipped(r)) return;
    // Pre-fix: a genuine AddressSanitizer stack-buffer-overflow at the
    // `entry.Dispose()` call site itself (r.ran=false), matching #4136's own
    // measured "[32, 40) ... offset 40 overflows this variable" report.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "key=7"));
}

BEZEL_TEST(dict_drain_entry_dispose_capture_4136,
           string_key_captured_loop_var_explicit_dispose_no_stack_overflow) {
    JitResult r = runAotCheckedLenient(stringKeyProgram());
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // 9: `entry.Value.N`, read before the explicit Dispose() under test.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "result=9"));
}

// ---------------------------------------------------------------------------
// Control: an UNCAPTURED DrainedEntry loop variable's explicit Dispose() --
// never hits the boxed branch of `boxedStorage`, so behaviour is
// byte-identical pre- and post-fix. Run under the STRICT runAotChecked
// (ASan+LSan) oracle: no capture cell is ever allocated for this shape, so
// unlike the two rows above there is no adjacent leak to route around.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_drain_entry_dispose_capture_4136,
           control_uncaptured_loop_var_explicit_dispose_unaffected) {
    const std::string src =
        "module M; public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Run(): int "
        "    using var d := new OwningDictionary<int, Widget>(); "
        "    d.Add(7, new Widget(9)); "
        "    var result := 0; "
        "    for var entry in d.Drain() do "
        "      result := entry.Value.N; "
        "      entry.Dispose(); "
        "    end; "
        "    return result; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine($\"result={C.Run()}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "result=9"));
}
