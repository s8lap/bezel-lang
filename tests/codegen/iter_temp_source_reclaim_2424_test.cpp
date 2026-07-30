// fix #2424 (§28 preamble/§28.2/§28.6/§28.9, §29.1, docs v0.235):
//
//   §28 preamble: "Every heap object created with `new` is disposed exactly
//   once" (leak prevention, the general obligation).
//
//   §29.1: "`GetEnumerator()` returns `owned IEnumerator<T>`: the caller
//   takes ownership of the enumerator and must dispose it" — establishing
//   that ownership of iteration-machinery objects is a first-class, tracked
//   obligation, not an afterthought.
//
// TWO independent manifestations of the same defect family: a TEMPORARY
// (unnamed, freshly-`new`'d) concrete-class object handed DIRECTLY to the
// §29 iterator machinery — with no other name anywhere binding it — was
// never reclaimed. Only a NAMED, independently-owned source got disposed
// (via its own separate scope-end disposal); a bare `new X(...)` used
// in-place as the coercion/iteration operand had no disposer at all.
//
// (A) `wrapUserClassAsIEnumerable`'s local-decl coercion (LlvmBackend_Assign
//     .cpp, invoked from LlvmBackend_Setup.cpp's `lowerLocalDecl`):
//       var s: IEnumerable<int> := new IntRange(1, 5);
//     leaked the `IntRange` instance unconditionally — nothing else named it,
//     and `wrapDisposeFn` deliberately never touched `srcObj` (a NAMED source
//     is independently disposed at ITS OWN scope end — see #2389's alias-
//     guard rationale) — but here there IS no independent name.
//
// (B) `lowerIEnumerableFor`'s desugaring (LlvmBackend_Stmts.cpp, the
//     `for var x in <concrete-class source> do ... end` path, #2384's
//     function):
//       for var x in new IntRange(1, 4) do sum := sum + x; end;
//     registered scope-end disposal ONLY for the enumerator it constructs;
//     nothing disposed the temporary SOURCE `new IntRange(1, 4)` produces.
//
// THE FIX. Both sites now classify the source exactly like the sibling
// array/collection paths already do (`lowerIEnumerableBuiltinFor`'s
// `sourceIsNamedLocal`, `registerIterableSourceDisposal`'s `wrappedHere`/
// `ownsSource`): resolve `ctx_.exprSymbol` on the source expression — a
// Local/Parameter/Field, or an AUTO-property, means an independent owner
// disposes it elsewhere and this site must NOT touch it (would double-free);
// anything else (a bare `new X(...)`, a plain method-call result) is an
// unowned temporary this site now becomes the SOLE disposer of.
//
//   (A)'s wrapper cannot bake this into the shared generated IR function —
//   `wrapUserClassAsIEnumerable`'s wrapper functions are CACHED per (source,
//   enumerator) class pair and reused across every call site that produces
//   that pair, some of which wrap a temporary and others a named source. The
//   fix threads `ownsSrc` through as a runtime DATA flag stored on each
//   wrapper INSTANCE (new struct slot 3), set at construction from the call
//   site's static classification, so the shared `wrapDisposeFn` reads
//   per-instance data instead of a per-function constant. A re-iteration
//   CLONE (`wrapGetEnumFn`, §29.11) always hardcodes its own flag to `false`
//   regardless of the original's value — only the ORIGINAL binding disposes
//   `src`, never a clone (multiple simultaneously-live enumerators over the
//   same temporary source must not multiply-free it).
//
//   (B)'s fix is simpler: `lowerIEnumerableFor` emits fresh IR per for-loop
//   statement (not a shared/cached helper), so the ownership decision is
//   baked in statically at that one call site — a plain conditional
//   `disposeStack_` registration, using the SAME generic class-dispose+
//   reclaim mechanism (`{slot, TypeSymbol}`) the enumerator's own entry
//   already uses. No new struct field or runtime flag needed here.
//
// ALIASING SAFETY (the single biggest risk per the fix brief — must not
// reopen #2389's double-free guard). In the self-returning idiom (#2384: one
// class implements BOTH `IEnumerable<T>` and `IEnumerator<T>`, returning
// `this` from `GetEnumerator()`), `enumPtr == srcObj` by runtime identity.
// (A)'s `wrapDisposeFn` therefore only dispatches+reclaims `srcObj` via the
// NEW `ownsSrc` path when NOT aliased (two-class idiom); when aliased, the
// EXISTING #2389 dispatch of `enumDisposeFn(enumPtr)` already ran `srcObj`'s
// own Dispose (same object, same disposer, since `enumTs == srcTs` whenever
// aliased) — only the physical reclaim (previously skipped entirely to avoid
// double-freeing an assumed-external owner) is now performed, exactly once,
// gated on `ownsSrc`. (B) needs no analogous branch at all: when
// `enumTs == ts` the EXISTING identity-guarded `enumDisposeSlot` registration
// already disposes the single shared object exactly once; the new source-
// disposal registration only fires when `enumTs != ts` (the two-class idiom,
// where `srcPtr` and `enumPtr` are always genuinely distinct objects).
//
// NOTE ON TRIVIAL-DISPOSE (per the fix brief's caveat — do not overclaim a
// normative violation). The issue's own repro classes (`IntRangeEnum`/
// `IntRange`, reused in tests A1/B1 below) have no destructor and no owned
// fields — §28.9 trivial-dispose, whose RECLAIM MECHANISM the spec leaves
// implementation-defined. The defect this fix closes is against THIS
// implementation's own chosen mechanism (explicit calloc/free, no GC — the
// #380 design commitment threaded through every other disposal site), not a
// narrow normative violation for these specific classes. Tests that need to
// prove EXACT-ONCE disposal (not just "no leak") use a COUNTED variant with a
// real destructor (`IntRangeCounted`/`SelfIterRes` below) — a live-instance
// counter distinguishes "disposed once" from "never disposed" (a leak) and
// from "disposed twice" (a double-free) far more precisely than a bare
// ASan/LSan pass can, exactly as issue #2384's own `Res`/`SelfIterRes`
// pattern does.
//
// ADVERSARIAL GRID COVERED (per the fix brief, weighted toward aliasing and
// non-temporary cases): temporary vs named local vs field vs using-var
// source; self-returning vs two-class idiom; for-in vs manual
// `.GetEnumerator()`; multiple enumerators simultaneously live over the same
// temporary source (must not multiply-free); early `break`; an exception
// thrown mid-iteration. Every "exactly-once" claim below is checked via a
// live-object counter, not merely "doesn't crash" / "no leak reported".

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
// LSan+ASan checked: -999 on ANY sanitizer report (leak/UAF/double-free/
// crash) or a non-clean run; the exit code otherwise. A passing
// BEZEL_EXPECT_EQ therefore proves both "no leak, no double-free" AND
// "correct value" in one assertion.
int runLeakChecked(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// The issue's own repro classes — TRIVIAL-DISPOSE (no destructor, no owned
// fields), matching #2424's/#2389's exact text.
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

// Same two-class idiom, but the SOURCE class carries a real destructor +
// static live-instance counter, so disposal is positively observable and
// EXACT-ONCE is checkable (not just "no leak reported").
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

// Self-returning idiom (#2384) with a real OWNED FIELD tracked via a static
// live-instance counter — mirrors iter_selfreturning_wrap_2384_test.cpp's
// `Res`/`SelfIterRes` exactly, so a double-dispose of the shared self-
// returned object is observable (a no-op destructor cannot expose one).
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

// =====================================================================
// Manifestation A: wrapUserClassAsIEnumerable's local-decl coercion
// (`var s: IEnumerable<int> := new X(...);`).
// =====================================================================

// A1 — THE ISSUE'S EXACT REPRO (trivial-dispose classes, LSan proof of no
// leak). Reproduces #2424's manifestation A verbatim.
BEZEL_TEST(iter_temp_source_reclaim_2424, a1_local_decl_temp_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    var s: IEnumerable<int> := new IntRange(1, 5); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// A1b — same shape, COUNTED source: proves the coercion's temporary is
// disposed EXACTLY ONCE (not merely "no leak reported"), and the enumerator
// still functions correctly through the wrapper.
BEZEL_TEST(iter_temp_source_reclaim_2424, a1b_local_decl_temp_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      var s: IEnumerable<int> := new IntRangeCounted(1, 5); "
        "      using var e := s.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// A2 — NAMED-SOURCE CONTROL: `var s: IEnumerable<int> := r;` where `r` is a
// separately-named, independently `using var`-disposed local. Must NOT
// double-free `r` — the #2389/#2424 fix's single biggest risk. A double-free
// either aborts under ASan (ran=false) or drives the counter negative; a
// leak (the fix regressing backward) would leave it positive. Only exactly 0
// passes.
BEZEL_TEST(iter_temp_source_reclaim_2424, a2_named_source_not_double_freed) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var r := new IntRangeCounted(1, 5); "
        "      var s: IEnumerable<int> := r; "
        "      using var e := s.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// A3 — SELF-RETURNING idiom (#2384) + TEMPORARY (owned) source, manual
// `.GetEnumerator()`. Exercises the aliasesSrc-AND-ownsSrc combination
// directly: `enumDisposeFn(enumPtr)` (existing #2389 dispatch) already runs
// the shared object's Dispose once (idempotent, §28.2); this fix's NEW
// `ownsSrc` branch must reclaim the block exactly once without re-dispatching
// Dispose a second time (would double-run the destructor even though the
// object's own `Res.n` flag-guard makes that survivable — still a §28.2
// "exactly once" violation the counter alone would not catch. It DOES catch
// a genuine double-free of the block itself, which the counter also cannot
// distinguish from a correct free until it crashes, hence runLeakChecked).
BEZEL_TEST(iter_temp_source_reclaim_2424, a3_selfreturning_temp_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      var s: IEnumerable<int> := new SelfIterRes(3); "
        "      using var e := s.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// A4 — MULTIPLE ENUMERATORS SIMULTANEOUSLY LIVE over the SAME temporary
// source (two-class idiom): the original wrapper (`s`) PLUS two independent
// `.GetEnumerator()` clones (`e1`, `e2`) all retain the shared source. Only
// the ORIGINAL may ever free it — a clone freeing it too would be the
// multiply-free hazard `wrapGetEnumFn`'s hardcoded `ownsSrc=false` exists to
// prevent. Disposed exactly once, proven via the counter.
BEZEL_TEST(iter_temp_source_reclaim_2424, a4_multiple_live_enumerators_no_multi_free) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      var s: IEnumerable<int> := new IntRangeCounted(1, 9); "
        "      using var e1 := s.GetEnumerator(); e1.MoveNext(); "
        "      using var e2 := s.GetEnumerator(); e2.MoveNext(); e2.MoveNext(); "
        "      sum := e1.Current * 10 + e2.Current; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // e1=1, e2=2 -> 12
        "  end Main; "
        "end C;"), 12);
}

// A5 — FIELD SOURCE CONTROL: the coercion's operand is a bare FIELD access
// (`f`), not a local. `ctx_.exprSymbol` must resolve it to a FieldSymbol so
// `srcIsNamedOwner=true` — the field's owner (`h`, an owned field of `s`'s
// enclosing object) disposes it via the §28.5 owned-field cascade; the
// coercion must NOT also free it.
BEZEL_TEST(iter_temp_source_reclaim_2424, a5_field_source_not_double_freed) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class Holder "
        "  public owned f: IntRangeCounted; "
        "  public constructor() f := new IntRangeCounted(1, 5); end constructor; "
        "  public method Wrap(): int "
        "    var s: IEnumerable<int> := f; "
        "    using var e := s.GetEnumerator(); "
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

// =====================================================================
// Manifestation B: lowerIEnumerableFor's for-in desugaring
// (`for var x in new X(...) do ... end`).
// =====================================================================

// B1 — THE ISSUE'S EXACT REPRO (trivial-dispose classes, LSan proof of no
// leak). Reproduces #2424's manifestation B verbatim; also the exact program
// iter_wrapdispose_reclaim_2389_test.cpp's
// `for_loop_over_concrete_source_value_unaffected` documents as leaking
// (that test intentionally uses the non-leak-checking `run()` because this
// fix did not exist yet — this test is the positive leak-free counterpart).
BEZEL_TEST(iter_temp_source_reclaim_2424, b1_forin_temp_two_class_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRange(1, 4) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;"), 10);
}

// B1b — same shape, COUNTED source: exactly-once disposal.
BEZEL_TEST(iter_temp_source_reclaim_2424, b1b_forin_temp_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRangeCounted(1, 4) do sum := sum + x; end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 10
        "  end Main; "
        "end C;"), 10);
}

// B2 — NAMED-SOURCE CONTROL: `for var x in r do ... end` over a separately
// `using var`-disposed `r`. Must NOT double-free.
BEZEL_TEST(iter_temp_source_reclaim_2424, b2_forin_named_source_not_double_freed) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var r := new IntRangeCounted(1, 4); "
        "      for var x in r do sum := sum + x; end; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 10
        "  end Main; "
        "end C;"), 10);
}

// B3 — SELF-RETURNING idiom (#2384) + TEMPORARY source, for-in directly over
// the concrete-typed temporary. `enumTs == ts` here, so this fix's NEW
// `enumTs != ts` gate does NOT fire — this test PINS that the pre-existing
// (already correct) identity-guarded path continues to dispose the shared
// object exactly once, now proven via the counter rather than bare LSan.
BEZEL_TEST(iter_temp_source_reclaim_2424, b3_forin_selfreturning_temp_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new SelfIterRes(3) do sum := sum + x; end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return sum; "  // 6
        "  end Main; "
        "end C;"), 6);
}

// B4 — TWO SEQUENTIAL for-loops, each over its OWN separate temporary
// (two-class idiom): confirms the fix disposes every distinct temporary
// source once each, not just the first / not just the last.
BEZEL_TEST(iter_temp_source_reclaim_2424, b4_forin_two_sequential_temporaries) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRangeCounted(1, 3) do sum := sum + x; end; "
        "    for var x in new IntRangeCounted(1, 3) do sum := sum + x; end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 6 + 6 = 12
        "  end Main; "
        "end C;"), 12);
}

// B5 — EARLY BREAK out of a for-loop over a temporary source: the disposal
// must fire on the `break` exit path (emitLoopDisposals over the SAME scope
// the normal-exhaustion path uses), not just on normal exhaustion.
BEZEL_TEST(iter_temp_source_reclaim_2424, b5_forin_break_disposes_temp_source) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRangeCounted(1, 100) do "
        "      if x = 3 then break; end; "
        "      sum := sum + x; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 1 + 2 = 3
        "  end Main; "
        "end C;"), 3);
}

// B6 — EXCEPTION thrown mid-iteration over a temporary source: the disposal
// must fire on the unwind path too, not just normal exit / break.
BEZEL_TEST(iter_temp_source_reclaim_2424, b6_forin_exception_disposes_temp_source) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeCounted +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    try "
        "      for var x in new IntRangeCounted(1, 100) do "
        "        if x = 3 then throw new InvalidOperationException(\"boom\"); end; "
        "        sum := sum + x; "
        "      end; "
        "    catch InvalidOperationException => "
        "      sum := sum + 100; "
        "    end; "
        "    if IntRangeCounted.live <> 0 then return 99; end; "
        "    return sum; "  // 1 + 2 + 100 = 103
        "  end Main; "
        "end C;"), 103);
}

// Value-correctness sanity (non-ASan) for both manifestations' repro shapes —
// a plain regression guard independent of the sanitizer harness.
BEZEL_TEST(iter_temp_source_reclaim_2424, values_correct_non_asan) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRange(1, 4) do sum := sum + x; end; "
        "    var s: IEnumerable<int> := new IntRange(1, 3); "
        "    using var e := s.GetEnumerator(); "
        "    if e.MoveNext() then sum := sum + e.Current; end; "
        "    return sum; "  // 10 + 1 = 11
        "  end Main; "
        "end C;"), 11);
}
