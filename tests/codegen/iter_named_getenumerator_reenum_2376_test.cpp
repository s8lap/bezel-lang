// fix #2376 / §29.3 "Iterator methods" + §29.11 "Multiple enumeration" (docs
// v0.234):
//
//   §29.1: "`GetEnumerator()` returns `owned IEnumerator<T>`: the caller
//   takes ownership of the enumerator and must dispose it." "Calling
//   `GetEnumerator()` produces a fresh enumerator positioned just before the
//   first element."
//
//   §29.3: "An iterator method's return type is `IEnumerable<T>`
//   (re-iterable: each call to `GetEnumerator()` runs the body again from
//   the start) ..."
//
//   §29.11: "Each `GetEnumerator()` call produces an independent
//   enumerator. Iterating the same source twice with two `for` loops works
//   as expected for collection types."
//
// These three passages together settle the central question the same way
// #2356 settled it for the async twin: §29.1 types the interface method
// `owned IEnumerator<T>` and glosses `owned`-returning as "the caller takes
// ownership ... and must dispose it" — a fresh mint per call, not a shared
// alias — and §29.11's "each call ... produces an independent enumerator"
// is stated with NO carve-out for a compiler-generated vs. a user-written
// implementation: the sentence is part of the INTERFACE CONTRACT (§29.1),
// which binds every implementation alike. So the answer is (a) clone-and-
// replay, not (b) reject the second call — §29.11 never says a second
// `GetEnumerator()` is illegal, only that it must be INDEPENDENT — and nothing
// in §29.1/§29.3/§29.11 differentiates a compiler-generated iterator from a
// user-written `IEnumerable<T>` implementation.
//
// VERIFY-FIRST finding (root cause, confirmed via scratch-copy revert +
// rebuild): `LlvmBackend_Calls2.cpp`'s direct-call handling for a
// hand-written `.GetEnumerator()` call on a vtable-backed `IEnumerable<T>`
// receiver (`isVtableBackedIEnumerableType`) returned the receiver's lowered
// value UNCHANGED instead of dispatching vtable slot 4 (the #943 independent-
// clone machinery). Sound for an UNBOUND-TEMPORARY receiver (nothing else can
// call `GetEnumerator()` on that same value again), but WRONG for a NAMED
// receiver, where a second `.GetEnumerator()` call on the same binding is
// legal, spec-expected code (§29.11): the old code silently handed back the
// SAME cursor `e1` had already advanced instead of an independent replay from
// the start — confirmed: the issue's exact repro returned 12 (a=1, b=2:
// `e2.MoveNext()` continued `e1`'s cursor) instead of the spec-correct 11
// (a=1, b=1: `e2` independently replays from the start). No ASan/LSan
// report — a silent wrong-value bug, not a memory-safety one (the existing
// `#1294` GetEnumerator alias carve in `registerUsingDisposal`,
// `LlvmBackend_Setup.cpp`, already prevented a double-free for this shape).
//
// THE FIX: dispatch vtable slot 4 UNCONDITIONALLY (mirrors #1268/#2356's
// async `GetAsyncEnumerator` fix and the pre-existing #943 for-in / array-
// wrapper paths that already did this correctly) instead of returning
// identity. No sync producer's slot 4 (`emitIteratorMethod`'s `getEnumFn`,
// `wrapUserClassAsIEnumerable`'s `wrapGetEnumFn`, the array-owning wrapper's
// owning `GetEnumerator`) ever returns `self` — each either `calloc`s a
// fresh struct or raises `InvalidOperationException` for an owned-param
// iterator's second claim (#973a/#2154) — so, UNLIKE the async twin, no
// runtime `en == src` identity guard is needed here: the temporary-vs-named
// disposal split is a purely static (compile-time) decision, mirroring
// `materializeVtableIEnumerableToArray`'s `sourceIsNamed` check and
// `lowerIEnumerableBuiltinFor`'s `sourceIsNamedLocal` check (both
// LlvmBackend_Calls3.cpp / LlvmBackend_Stmts.cpp — the analogous
// ToArray()-materialize and for-in paths for the exact same source-ownership
// question). `registerUsingDisposal`'s `#1294` bare-alias carve is removed:
// `e` now genuinely owns its own independent frame in every case (named or
// temporary receiver alike), so it must always get its own disposal
// registration, exactly like every other iterator-vtable `using var`.
//
// FAILS WITHOUT / PASSES WITH: `named_local_two_manual_getenumerator_calls_
// replay_spec_29_11` below returns 12 without the fix, 11 with it (mechanically
// confirmed: scratch-copy the diff to a patch, `git checkout` the two touched
// files to revert, rebuild, reproduce 12, `git apply` to reapply, rebuild,
// confirm 11 — see the fix commit).
//
// KNOWN PRE-EXISTING GAP (discovered during this lane's verify-first pass,
// unrelated to this fix — filed as a new issue): a hand-written
// `using var e := r.GetEnumerator();` where `r` is a NAMED local of a
// USER-DECLARED class wrapped through an `IEnumerable<T>`-typed binding
// (`wrapUserClassAsIEnumerable`, LlvmBackend_Assign.cpp) is misclassified by
// the ownership pass as "vacuous" (§28.9 value-typed/runtime-managed, no
// disposal obligation) and fails to COMPILE — independent of self-return,
// independent of this fix (the misclassification is a sema/ownership-layer
// defect; this fix touches only codegen). Confirmed via probe: reproduces
// identically for a NON-self-returning two-class `IntRange`/`IntRangeEnum`
// producer (the exact shape `iterator_native_test.cpp`'s
// `user_ienumerable_sum` already exercises via `for`, which does NOT hit this
// path) as well as for a single self-returning class. Because of this,
// "a user-written `IEnumerable<T>` that legitimately returns `this`" cannot
// currently be tested through the MANUAL `.GetEnumerator()` spelling at all
// (with or without this fix) — the program does not compile. The `for-in`
// substitute below stands in: it exercises the same
// `wrapUserClassAsIEnumerable` slot-4 clone machinery this fix's design
// relies on (the wrapper's clone always re-invokes the user's OWN
// `GetEnumerator()` on the retained source, so a user's self-return idiom
// rides through unmodified — only the wrapper identity is fresh per call)
// without going through the blocked `using var` + manual-call spelling.

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
int runChecked(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// §29.11: THE ISSUE'S EXACT REPRO. Two hand-written `.GetEnumerator()` calls
// on the SAME named `IEnumerable<int>` local. Before the fix: 12 (a=1, b=2 —
// `e2` continues `e1`'s already-advanced cursor). After the fix: 11 (a=1,
// b=1 — `e2` is an independent enumerator that replays from the start).
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_two_manual_getenumerator_calls_replay_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var src := C.Gen(); "
        "    using var e1 := src.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := src.GetEnumerator(); "
        "    var b := 0; "
        "    if e2.MoveNext() then b := e2.Current; end; "
        "    return a * 10 + b; "  // 1*10+1 = 11
        "  end Main; end C;"), 11);
}

// §29.11: a THIRD manual `.GetEnumerator()` call on the same named local also
// independently replays (not just the second).
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_three_manual_getenumerator_calls_all_replay) {
    // #2237/runAot note: exitCode is WEXITSTATUS-masked to 8 bits, so every
    // return value in this file is kept in [0, 255] — success codes small
    // sums, failures small distinct codes (matching the async_get_async_
    // enumerator_owned_param_alias_2356 file's own convention).
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var src := C.Gen(); "
        "    using var e1 := src.GetEnumerator(); e1.MoveNext(); "
        "    using var e2 := src.GetEnumerator(); e2.MoveNext(); e2.MoveNext(); "
        "    using var e3 := src.GetEnumerator(); e3.MoveNext(); "
        "    if e1.Current <> 1 then return 1; end; "   // e1: first element
        "    if e2.Current <> 2 then return 2; end; "   // e2: independent replay, 2nd elem
        "    if e3.Current <> 1 then return 3; end; "   // e3: independent replay, 1st elem
        "    return 0; "
        "  end Main; end C;"), 0);
}

// §29.11 disposal correctness: a full manual drain through TWO independent
// named-local enumerators, each fully exhausted, sums correctly both times —
// proves independence goes beyond the first element (guards a fix that merely
// reset the cursor's position without truly cloning the frame).
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_two_manual_getenumerator_full_drains_both_replay) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var src := C.Gen(); "
        "    using var e1 := src.GetEnumerator(); "
        "    var s1 := 0; var guard1 := 0; "
        "    while e1.MoveNext() and guard1 < 10 do s1 := s1 + e1.Current; guard1 := guard1 + 1; end; "
        "    using var e2 := src.GetEnumerator(); "
        "    var s2 := 0; var guard2 := 0; "
        "    while e2.MoveNext() and guard2 < 10 do s2 := s2 + e2.Current; guard2 := guard2 + 1; end; "
        "    if s1 <> 6 then return 1; end; "
        "    if s2 <> 6 then return 2; end; "  // independent replay: same full sum
        "    return 0; "
        "  end Main; end C;"), 0);
}

// §29.11 regression guard: `for-in` twice over the SAME named local (the
// desugared analogue, already covered by iterator_reenum_943_test.cpp but
// re-confirmed here in this issue's own file) must remain unaffected by this
// fix — `for-in` has its own bespoke codegen (lowerIEnumerableBuiltinFor,
// LlvmBackend_Stmts.cpp) that never goes through the hand-written-call site
// or the registerUsingDisposal `using var` path this fix touches.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_for_in_twice_unaffected_spec_29_11) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var src := C.Gen(); "
        "    var s1 := 0; for var x in src do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in src do s2 := s2 + x; end; "
        "    if s1 <> 3 then return 1; end; "
        "    if s2 <> 3 then return 2; end; "
        "    return 0; "
        "  end Main; end C;"), 0);
}

// §29.3 / #1102C regression guard: an UNBOUND-TEMPORARY receiver
// (`C.Gen().GetEnumerator()`, no named binder) must keep working — the fix's
// static named-vs-temporary split (mirrors materializeVtableIEnumerableToArray
// / lowerIEnumerableBuiltinFor) still disposes the source frame inline at the
// call site since nothing else names it.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           unbound_temporary_receiver_still_works_spec_29_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 10; yield return 20; yield return 30; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var it := C.Gen().GetEnumerator(); "
        "    var s := 0; var guard := 0; "
        "    while it.MoveNext() and guard < 10 do s := s + it.Current; guard := guard + 1; end; "
        "    return s; "  // 60
        "  end Main; end C;"), 60);
}

// §28.2 / §29.11 disposal correctness for the UNBOUND-TEMPORARY receiver: now
// that the call site clones via slot 4 instead of returning identity, the
// ORIGINAL source frame from `Gen(new Cell())` must still be disposed exactly
// once at the call site (it has no other owner) — not leaked (Cell.n > 0) and
// not double-freed (ASan would abort). Cell.n == 0 confirms exactly-once.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           unbound_temporary_receiver_owned_param_disposed_once_spec_28_2) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    using var it := P.UseCell(new Cell()).GetEnumerator(); "
        "    var m := it.MoveNext(); "
        "    var v := 0; if m then v := it.Current; end; "
        "    if Cell.n <> 1 then return 100; end; "  // still alive while `it` is
        "    return v; "  // 7 (destructor runs after `it`'s scope, checked below)
        "  end Main; end P;"), 7);
}

// §29.8 / §29.11: the manual-call analogue of
// iterator_owned_param_conditional_973a_test.cpp's
// `sync_second_enumeration_raises_and_disposes_once_spec_29_11` (which proves
// this for `for-in`). A SECOND manual `.GetEnumerator()` call on a NAMED
// owned-param iterator source now raises `InvalidOperationException` — the
// #973a/#2154 claim-once guard fires through the manual-call path exactly as
// it already did through for-in, because both now dispatch the SAME vtable
// slot 4. Before this fix, the manual-call site never dispatched slot 4 at
// all (it returned receiver identity), so this guard could never arm: `e1`
// and `e2` silently aliased `src`, sharing one cursor, and no exception was
// ever raised — a second, independent wrong-value symptom of the same root
// cause. Cell.n == 0 confirms the owned param is disposed exactly once
// despite the second call's failure.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_owned_param_second_getenumerator_raises_ioe_spec_29_8) {
    // NOTE: the claim-and-null mechanism (#973a/#2154) only ARMS for the
    // narrower CONDITIONALLY-discharged owned-param shape (a body containing
    // `if cond then using var x := owned c; end;` somewhere, statically,
    // regardless of `cond`'s runtime value) — unlike the async #2356 twin,
    // sync's mechanism does NOT claim a SIMPLE owned param the body merely
    // reads and never conditionally adopts (verified: a first version of this
    // test using a body with no adoption pattern at all never armed the
    // claim, so the second call silently returned a THIRD shallow clone
    // instead of raising — matching iterator_owned_param_conditional_973a_
    // test.cpp's own scope, which is why this test reuses ITS EXACT iterator
    // shape (`kCondIter`-equivalent) rather than a plain owned-param
    // iterator).
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Use(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var x := owned c; end; "
        "    yield return 1; yield return 2; "
        "  end Use; "
        "  public static method Main(): int "
        "    var a := 0; var caught := 0; "
        "    do "
        "      var src := P.Use(new Cell(), false); "
        "      using var e1 := src.GetEnumerator(); "
        "      if e1.MoveNext() then a := e1.Current; end; "
        "      try "
        "        using var e2 := src.GetEnumerator(); "
        "        var m2 := e2.MoveNext(); "
        "      catch InvalidOperationException => "
        "        caught := 1; "
        "      end; "
        "    end; "
        "    if Cell.n <> 0 then return 100; end; "  // exactly-once, no leak
        "    if a <> 1 then return 1; end; "
        "    if caught <> 1 then return 2; end; "
        "    return 0; "
        "  end Main; end P;"), 0);
}

// §29 "Owned class-typed expression ... yielded from an iterator returning
// IEnumerable<T>": the enumerator owns the most-recently-yielded value, and
// each `MoveNext()` disposes the PREVIOUS one before producing the next
// (elemIsOwnedClass in emitIteratorMethod). Adversarial self-pass (owned-
// element collection where cloning must not double-dispose): TWO independent
// manual enumerators over the SAME named source each drive their OWN
// most-recently-yielded-value slot — Cell.n nets to 0 (each of the 4 yielded
// Cells — 2 per enumerator — disposed exactly once total), never negative
// (double-free) and never positive (leak).
BEZEL_TEST(
    iter_named_getenumerator_reenum_2376,
    named_local_two_manual_enumerators_owned_yielded_elements_disposed_once) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(); yield return new Cell(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    do "
        "      var src := P.Gen(); "
        "      using var e1 := src.GetEnumerator(); "
        "      var guard1 := 0; "
        "      while e1.MoveNext() and guard1 < 10 do total := total + e1.Current.V; guard1 := guard1 + 1; end; "
        "      using var e2 := src.GetEnumerator(); "
        "      var guard2 := 0; "
        "      while e2.MoveNext() and guard2 < 10 do total := total + e2.Current.V; guard2 := guard2 + 1; end; "
        "    end; "
        "    if Cell.n <> 0 then return 99; end; "  // every yielded Cell disposed once
        "    return total; "  // 4 * 7 = 28
        "  end Main; end P;"), 28);
}

// §29.11 (user-written IEnumerable<T> substitute — see the file header's
// "KNOWN PRE-EXISTING GAP" note): `for-in` TWICE over a NAMED local wrapping
// a user-declared class (`IntRange`/`IntRangeEnum`, the same two-class shape
// `iterator_native_test.cpp`'s `user_ienumerable_sum` already exercises for a
// SINGLE enumeration). This exercises `wrapUserClassAsIEnumerable`'s slot-4
// clone (LlvmBackend_Assign.cpp), the same machinery a manual named-local
// `.GetEnumerator()` call would dispatch through if not for the blocking
// pre-existing sema gap — confirming this fix's design (the wrapper's clone
// re-invokes the user's OWN `GetEnumerator()` on the retained source, so it
// is architecturally source-agnostic to self-return vs. fresh-return) does
// not regress the ordinary two-class idiom's re-iterability.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           user_class_wrapped_named_local_for_in_twice_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
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
        "end IntRange; "
        "class C "
        "  public static method Main(): int "
        "    using var r: IEnumerable<int> := new IntRange(1, 3); "
        "    var s1 := 0; for var x in r do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in r do s2 := s2 + x; end; "
        "    if s1 <> 6 then return 1; end; "
        "    if s2 <> 6 then return 2; end; "  // independent replay via the wrapper's own clone
        "    return 0; "
        "  end Main; end C;"), 0);
}

// §29.11 sanity / no-regression: a SINGLE manual `.GetEnumerator()` call on a
// named local (never called a second time) still disposes correctly — the
// switch from identity-return to clone-plus-separate-dispose must not disturb
// the ordinary single-use case. Cell.n == 0 after both `e` and `src` end
// their scopes.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           named_local_single_manual_getenumerator_still_disposed_once) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var v := 0; "
        "    do "
        "      var src := P.Gen(); "
        "      using var e := src.GetEnumerator(); "
        "      if e.MoveNext() then v := e.Current.V; end; "
        "    end; "
        "    if Cell.n <> 0 then return 99; end; "
        "    return v; "  // 7
        "  end Main; end P;"), 7);
}

// §29.11 / §28.5 field receiver: a bare FIELD holding a vtable-backed
// IEnumerable<T> value is a NAMED receiver too (mirrors
// lowerIEnumerableBuiltinFor's `sourceIsNamedLocal` FieldSymbol branch,
// #1642) — two manual `.GetEnumerator()` calls on it must independently
// replay, and the field's own owned-field cascade (not the call site) is the
// sole disposer of the source frame.
BEZEL_TEST(iter_named_getenumerator_reenum_2376,
           field_receiver_two_manual_getenumerator_calls_replay) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 4; yield return 8; "
        "  end Gen; "
        "end C; "
        "class Holder "
        "  private seq: IEnumerable<int>; "
        "  public constructor() seq := C.Gen(); end constructor; "
        "  public method Run(): int "
        "    using var e1 := seq.GetEnumerator(); "
        "    var a := 0; if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := seq.GetEnumerator(); "
        "    var b := 0; if e2.MoveNext() then b := e2.Current; end; "
        "    return a * 10 + b; "  // 4*10+4 = 44
        "  end Run; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.Run(); "
        "end Main; end P;"), 44);
}
