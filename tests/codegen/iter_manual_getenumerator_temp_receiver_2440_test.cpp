// fix #2440 (§28 preamble/§28.2/§28.6/§28.9, §29.1, docs v0.235):
//
// A fourth call site of `wrapUserClassAsIEnumerable` — the manual
// `.GetEnumerator()` call on a bare CONCRETE-class-typed receiver
// (LlvmBackend_Calls2.cpp:4086, #2388's fix site) — independently leaked a
// TEMPORARY receiver, discovered while sweeping #2424's fix for the same
// call-site shape. `new IntRange(1, 5).GetEnumerator()` leaked the `IntRange`
// receiver: nothing else names it, and (per #2424's root-cause analysis) a
// NAMED receiver is independently disposed at its own scope end, but this
// call site's `wrapUserClassAsIEnumerable(...)` invocation was left at the
// new `ownsSrc` parameter's safe default (`false`) when #2424 landed — #2424
// only fixed the local-decl coercion and for-in desugaring sites, and
// recorded this one (plus `wrapAsIEnumerableForTarget`'s return/argument
// site, NOT reproduced) in issue #2440 rather than expanding that commit.
//
// THE FIX. Same technique as #2424: classify `ma->target` (the
// `.GetEnumerator()` call's receiver expression) via `ctx_.exprSymbol` —
// Local/Parameter/Field/auto-property means an independent owner disposes it
// elsewhere; anything else is an unowned temporary this call site now passes
// `ownsSrc=true` for. The classification snippet is copied VERBATIM from the
// already-shipped, sibling #2376 fix a few dozen lines below in the same
// `.GetEnumerator()`-handling block (the `isVtableBackedIEnumerableType`
// branch's `sourceIsNamed` test) — both are classifying the receiver of a
// `.GetEnumerator()` call for the identical reason, so this is proven
// machinery, not a fresh re-derivation. The one structural difference: #2376's
// branch disposes `src` via a SEPARATE disposeStack_ registration (its slot-4
// dispatch always returns a genuine, distinct clone), whereas this site's
// `src` is CONSUMED directly into the one wrapper struct the caller will
// dispose — so classification feeds `wrapUserClassAsIEnumerable`'s `ownsSrc`
// runtime struct flag (§2424) instead.
//
// COORDINATOR REVIEW NOTES ADDRESSED (do not assume — verify at THIS site):
//   * "ctx_.exprSymbol may behave differently for a method-call receiver
//     than a local-decl initializer" — not assumed: d2/d3 below are
//     leak-AND-double-free-checked (runLeakChecked) NAMED-receiver controls
//     at THIS exact call site, and every EXISTING #2389/#2388/#2384
//     leak-checked test using a named manual-`.GetEnumerator()` receiver
//     (iter_wrapdispose_reclaim_2389, iter_concrete_manual_getenumerator_2388,
//     iter_selfreturning_wrap_2384) was re-run green after this change with
//     NO modification — a wrong classification would have flipped a NAMED
//     receiver to ownsSrc=true and immediately double-freed under ASan there.
//   * "multiply-free axis: several enumerators from a temporary receiver,
//     and re-iteration" — d6 stresses many independent temporaries through
//     this exact site (the shared, CACHED wrapDisposeFn/wrapGetEnumFn
//     functions invoked repeatedly with fresh instances); d7 additionally
//     interleaves a NAMED-receiver call and a TEMPORARY-receiver call to the
//     SAME (source, enumerator) class pair THROUGH THIS SITE in one program,
//     proving `ownsSrc` is genuinely per-INSTANCE data (struct slot 3, per
//     #2424) and not accidentally conflated by the function-level cache the
//     widened `srcTs$enumTs` key protects. True re-iteration (§29.11 slot-4
//     cloning) is not independently retested here: `IEnumerator<T>` has no
//     `GetEnumerator()` of its own (stdlib §4.12) — nothing in this specific
//     call's OWN result type exposes a re-iteration surface — so the
//     multiply-free protection at play is entirely the clone-hardcodes-false
//     guarantee #2424 already built and tested (iter_temp_source_reclaim_2424
//     a4); d7 is this site's most direct analogue.
//
// TRIVIAL-DISPOSE CAVEAT: same as #2424 — the issue's own repro classes are
// §28.9 trivial-dispose; tests needing an EXACT-ONCE proof (not just "no
// leak") use a counted variant with a real destructor and a live-instance
// counter.
//
// DISCOVERED, NOT FIXED HERE, FILED AS #2443: the self-returning idiom
// (#2384) at THIS call site — `using var e := <SelfIterRes-shaped>
// .GetEnumerator();` — leaks the wrapper UNCONDITIONALLY (named or temporary
// receiver alike), for a reason unrelated to this fix (a pre-existing
// ownership-pass misclassification, confirmed present with only #2424
// applied). See the comment above d6 below for the full analysis. This file
// therefore has no self-returning-idiom test — d1/d1b/d2/d3/d6/d7 all use
// the two-class idiom, the one shape this fix's classification and #2440's
// repro actually exercise cleanly.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
int runLeakChecked(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// The issue's own repro classes — trivial-dispose.
const std::string kIntRangeTwoClass =
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

// Counted variant — real destructor + live-instance counter.
const std::string kIntRangeCounted =
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "class IntRangeCounted: IEnumerable<int> "
    "  public static live: int := 0; "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo:=lo; this.hi:=hi; IntRangeCounted.live := IntRangeCounted.live + 1; "
    "  end constructor; "
    "  destructor IntRangeCounted.live := IntRangeCounted.live - 1; end destructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
    "end IntRangeCounted; ";

// Self-returning idiom, owned-field counted (mirrors #2384's Res/SelfIterRes).
const std::string kSelfIterRes =
    "class Res public static n: int := 0; "
    "  public constructor() Res.n := Res.n + 1; end constructor; "
    "  destructor Res.n := Res.n - 1; end destructor; "
    "end Res; "
    "class SelfIterRes: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; private owned r: Res; "
    "  public constructor(hi: int) this.hi := hi; r := new Res(); end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfIterRes; ";
}  // namespace

// d1 — THE ISSUE'S EXACT REPRO (trivial-dispose, LSan proof of no leak).
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d1_temp_receiver_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var e := new IntRange(1, 5).GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "
        "  end Main; "
        "end C;"), 1);
}

// d1b — same shape, COUNTED: exactly-once disposal of the temporary receiver.
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d1b_temp_receiver_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var e := new IntRangeCounted(1, 5).GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// d2 — NAMED LOCAL receiver control at THIS site: must NOT double-free. The
// coordinator's specific concern (classification correctness at a
// method-call receiver, not assumed to mirror lowerLocalDecl). A wrong
// classification here would flip `r` to ownsSrc=true and double-free it
// against its own `using var` disposal — caught by ASan, not just the
// counter.
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d2_named_local_receiver_not_double_freed) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var r := new IntRangeCounted(1, 5); "
        "      using var e := r.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// d3 — FIELD receiver control: `f.GetEnumerator()` where `f` is a bare
// (unqualified) owned field access, not a local-decl initializer.
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d3_field_receiver_not_double_freed) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class Holder "
        "  public owned f: IntRangeCounted; "
        "  public constructor() f := new IntRangeCounted(1, 5); end constructor; "
        "  public method Wrap(): int "
        "    using var e := f.GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "
        "  end Wrap; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var r := 0; "
        "    do "
        "      using var h := new Holder(); "
        "      r := h.Wrap(); "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return r; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// d4/d5 — SELF-RETURNING idiom (#2384) at this site, temporary AND named
// receiver, were both ATTEMPTED here and both failed — but NOT because of
// anything this fix touches. `using var e := <SelfIterRes-shaped>
// .GetEnumerator();` is unconditionally leaked (the WRAPPER itself is never
// disposed, "Direct leak of 32 byte(s)... in main") regardless of whether
// the receiver is named or temporary, and reproduces identically with only
// #2424's fix present (before this commit's classification change) —
// mechanically confirmed via scratch-copy revert of this commit's
// LlvmBackend_Calls2.cpp diff. Root cause: the OWNERSHIP PASS's
// `usingVarInitIsRecognizedBorrow` (src/ownership/Ownership.cpp, the
// `CallExpr` / `methodReturnsOnlyReceiverSourcedBorrows` branch, #2137(b))
// positively recognizes ANY directly-resolved method whose every return is a
// bare `return this;` as a borrow-pin source — correct for an ordinary
// receiver-returning method, but `GetEnumerator(): IEnumerator<int> return
// this; end GetEnumerator;` on a self-returning `IEnumerable<T>`+
// `IEnumerator<T>` class matches the same shape even though the CALL
// expression's value is actually a freshly calloc'd wrapper (this site's
// own `wrapUserClassAsIEnumerable`), which the ownership pass's static,
// body-shape-only analysis has no visibility into. `registerUsingDisposal`
// then honors the (incorrectly-set) `usingVarSourceIsBorrow` flag and
// registers no disposal at all. This is a DIFFERENT, deeper defect than
// #2424/#2440 (which are about whether an ALREADY-disposed wrapper's
// disposal additionally frees the retained source) — here the wrapper's own
// disposal never fires in the first place, unconditionally, for both named
// and temporary receivers alike. Filed as #2443 with the confirmed root
// cause and suggested fix direction (spans the ownership pass and codegen —
// not a quick fix); not attempted in this commit.

// d6 — MANY independent temporary receivers through this exact site, in a
// loop: stresses the shared, CACHED wrapDisposeFn/wrapGetEnumFn functions
// invoked repeatedly with fresh per-instance ownsSrc data, proving no
// cross-instance leakage or corruption of the flag across many allocations.
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d6_many_independent_temp_receivers) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 5 do "
        "      using var e := new IntRangeCounted(1, 3).GetEnumerator(); "
        "      if e.MoveNext() then sum := sum + e.Current; end; "
        "      i := i + 1; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 1*5 = 5
        "  end Main; "
        "end C;"), 5);
}

// d7 — CROSS-SITE / CROSS-INSTANCE isolation: a NAMED-receiver call and a
// TEMPORARY-receiver call to the SAME (source, enumerator) class pair
// THROUGH THIS SAME SITE, interleaved in one program. Both share the
// identical cached wrapDisposeFn/wrapGetEnumFn (per #2424's widened
// `srcTs$enumTs` cache key) — this proves `ownsSrc` is genuinely per-
// INSTANCE data, not a function-level constant that would apply the wrong
// verdict to whichever call happened to compile the shared function first.
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, d7_named_and_temp_share_cached_wrapper_functions) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var r := new IntRangeCounted(10, 12); "
        "      using var eNamed := r.GetEnumerator(); "
        "      if eNamed.MoveNext() then sum := sum + eNamed.Current; end; "  // 10
        "      using var eTemp := new IntRangeCounted(1, 3).GetEnumerator(); "
        "      if eTemp.MoveNext() then sum := sum + eTemp.Current; end; "   // 1
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 11
        "  end Main; "
        "end C;"), 11);
}

// Value-correctness sanity (non-ASan).
BEZEL_TEST(iter_manual_getenumerator_temp_receiver_2440, values_correct_non_asan) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var e := new IntRange(1, 5).GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    if e.MoveNext() then a := a + e.Current; end; "
        "    return a; "  // 1 + 2 = 3
        "  end Main; "
        "end C;"), 3);
}
