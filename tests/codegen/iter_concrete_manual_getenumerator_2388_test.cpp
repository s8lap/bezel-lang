// fix #2388 (§11.2 "Member matching" / §29.1 "The protocol" / §29.11
// "Multiple enumeration", docs v0.235):
//
//   §29.1: "Code that calls `GetEnumerator()` manually — outside a loop
//   form — obtains an owned enumerator subject to the standard §28.3
//   discharge rules; the `using var x := source.GetEnumerator();` shape is
//   recommended..."
//
//   §29.11: "Each `GetEnumerator()` call produces an independent
//   enumerator."
//
//   §11.2 ("Interface declaration"): a member "matches" an interface member
//   when, among other things, its return type is "exactly equal under the
//   interface's substitution" — the reason a user's `GetEnumerator(): owned
//   IEnumerator<T>` can only ever be spelled with the bare builtin
//   `IEnumerator<T>`, never a concrete class name, even though the runtime
//   object it returns is a concrete class instance.
//
// ROOT CAUSE. `s.GetEnumerator()` called manually (not via `for`) on a
// receiver whose STATIC type is a CONCRETE user class (e.g. `s: IntRange`,
// not `s: IEnumerable<int>`) fell through every existing special case in
// `Emitter::lowerCall` (LlvmBackend_Calls2.cpp) — those all require a
// declaration-LESS receiver type (Array<T>, or an already vtable-backed
// IEnumerable<T>) — straight through to the ordinary resolved-MethodSymbol
// call path. That path invokes the user's `GetEnumerator()` and hands back
// the RAW enumerator class instance UNWRAPPED, carrying its own ordinary
// CG35-itable class vtable ([0]=td,[1]=itmap$,[2]=Dispose,[3]=ToString,...).
// Every consumer of an `IEnumerator<T>`-typed value — the #494 `MoveNext()`
// dispatch, the `TakeCurrent()` dispatch, and the `iterEnumDispose`
// scope-end disposal a `using var e := …` wires up via
// `registerUsingDisposal` — unconditionally reads vtable slot 1/2/3
// assuming the DIFFERENT §29.11 iterator-vtable format ([0]=td,[1]=Dispose,
// [2]=MoveNext,[3]=Current,[4]=GetEnumerator; the format
// `wrapUserClassAsIEnumerable`/`wrapArrayAsIEnumerable`/`emitIteratorMethod`
// all produce). Reading slot 1 of the RAW class's ordinary vtable — which
// holds `itmap$<Enum>` (a DATA pointer, not code) at that position, not a
// function — and calling it: a wild jump landing inside the `itmap$`
// global itself. Confirmed via ASan: "SEGV ... in itmap$IntRangeEnum",
// "PC is at a non-executable region. Maybe a wild jump?" — exactly the
// issue's repro.
//
// THE FIX (LlvmBackend_Calls2.cpp, `Emitter::lowerCall`): intercept
// `s.GetEnumerator()` right after the callee `MethodSymbol` is resolved
// (before the `if (!ms)` special-casing that only ever sees
// declaration-less builtin receivers), when the receiver's static type is a
// concrete class AND the call's own sema-inferred result type is the
// declaration-less builtin `IEnumerator<T>`/`IOwningEnumerator<T>` sentinel
// (ruling out an unrelated same-named method). Reuse
// `wrapUserClassAsIEnumerable` (already used for `IEnumerable<T>`
// coercions, #552/#2384) to build the SAME normalized iterator-vtable
// wrapper a manual call on an `IEnumerable<T>`-typed receiver already gets,
// so every consumer downstream sees the one representation it expects.
//
// COMPANION SILENT-WRONG-VALUE BUG (same root cause, no crash): a
// self-returning class (`GetEnumerator()` returns `this`) called manually
// on a concrete-typed receiver used to silently return the WRONG value
// (misread garbage past the small itmap$/vtable header, non-fatal only by
// chance of that class's particular layout) instead of crashing. Fixed by
// the same change (`self_returning_class_manual_call_correct_value` below).
//
// FAILS WITHOUT / PASSES WITH: every test below reproduces the issue's
// SEGV (or, for the self-returning variant, the silent wrong value) when
// LlvmBackend_Calls2.cpp is reverted via scratch-copy `git checkout` — the
// program crashes under ASan/the JIT-oracle equivalent, or returns a wrong
// value; rebuilding with the fix in place is required for these to compile,
// run, and return the asserted value at all (mechanically confirmed during
// development — see the fix commit).
//
// Uses plain `runAot` (no ASan/LSan leak net): this test pins the CRASH /
// WRONG-VALUE defect only. The companion leak defect in the SAME
// `wrapUserClassAsIEnumerable` wrapper this fix routes through is #2389,
// pinned separately (iter_wrapdispose_reclaim_2389_test.cpp) — keeping the
// two nets apart means either fix can be reverted independently without
// this file's tests changing meaning.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// The two-class idiom from the issue's own repro.
const char* kIntRangeTwoClass =
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "class IntRange: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
    "end IntRange; ";
}  // namespace

// THE ISSUE'S EXACT REPRO: `s` is CONCRETE-typed (`using var s :=
// new IntRange(...)`, no `IEnumerable<int>` annotation), `.GetEnumerator()`
// called manually. Pre-fix: ASan SEGV in itmap$IntRangeEnum. Post-fix: a=1
// (MoveNext advances cur to lo=1, 1<=hi=3 is true; Current=1).
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, issue_exact_repro_movenext_current) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 3); "
        "    using var e1 := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    return a; "
        "  end Main; "
        "end C;"), 1);
}

// §29.11 independence: TWO manual `.GetEnumerator()` calls on the SAME
// concrete-typed NAMED receiver must produce INDEPENDENT enumerators, each
// starting fresh — not aliasing each other or `s`'s own state.
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, two_calls_independent_replay) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 5); "
        "    using var e1 := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := s.GetEnumerator(); "
        "    var b := 0; "
        "    if e2.MoveNext() then b := e2.Current; end; "
        "    return a * 10 + b; "  // both independently see the FIRST element: 1*10+1 = 11
        "  end Main; "
        "end C;"), 11);
}

// Adversarial: a full manual drain (MoveNext + Current loop, not just one
// step) sums every element correctly across MULTIPLE advances — exercises
// the wrapper's MoveNext/Current dispatch repeatedly, not just once.
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, full_manual_drain_sums) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 4); "
        "    using var e := s.GetEnumerator(); "
        "    var sum := 0; var guard := 0; "
        "    var more := e.MoveNext(); "
        "    while more and guard < 100 do "
        "      sum := sum + e.Current; "
        "      more := e.MoveNext(); "
        "      guard := guard + 1; "
        "    end; "
        "    return sum; "  // 1+2+3+4 = 10
        "  end Main; "
        "end C;"), 10);
}

// Adversarial: MoveNext() returns false past exhaustion for a manual call
// on a concrete-typed source (empty-range boundary).
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, exhausted_movenext_false) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(5, 3); "  // lo > hi: empty
        "    using var e := s.GetEnumerator(); "
        "    if e.MoveNext() then return 1; else return 7; end; "
        "  end Main; "
        "end C;"), 7);
}

// Companion silent-wrong-value bug: a SELF-RETURNING class (implements both
// IEnumerable<int> and IEnumerator<int>, `GetEnumerator()` returns `this`)
// called manually on a CONCRETE-typed receiver. Pre-fix this did not crash
// (non-fatal only by chance of layout) but silently returned 0 instead of
// 1; post-fix it must return the correct value.
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, self_returning_class_manual_call_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class SelfIter: IEnumerable<int>, IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
        "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end SelfIter; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new SelfIter(1, 3); "
        "    using var e := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "
        "  end Main; "
        "end C;"), 1);
}

// Regression guard: `for` over a CONCRETE-typed source (a different codegen
// path entirely — `lowerIEnumerableFor`, LlvmBackend_Stmts.cpp, #2384) is
// untouched by this fix and must keep working.
BEZEL_TEST(iter_concrete_manual_getenumerator_2388, for_loop_over_concrete_source_unaffected) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRange(1, 4) do sum := sum + x; end; "
        "    return sum; "  // 1+2+3+4 = 10
        "  end Main; "
        "end C;"), 10);
}
