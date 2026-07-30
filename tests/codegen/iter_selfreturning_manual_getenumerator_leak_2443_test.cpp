// fix #2443 (§29.1 "The protocol" / §28.10 "using var" — the "using var
// initialized from a borrow" subsection, docs v0.235):
//
//   §29.1 (the `IEnumerable<T>`/`IEnumerator<T>` protocol declaration and its
//   prose): "`GetEnumerator()` returns `owned IEnumerator<T>`: the caller
//   takes ownership of the enumerator and must dispose it... Code that calls
//   `GetEnumerator()` manually — outside a loop form — obtains an owned
//   enumerator subject to the standard §28.3 discharge rules; the `using var
//   x := source.GetEnumerator();` shape is recommended."
//
//   §28.10's "using var initialized from a borrow" subsection: "When the
//   right-hand side of a `using var` declaration is a borrow rather than an
//   owned value (a method call without an `owned` return... ), the `using
//   var` does not assume a disposal obligation... at scope end, no
//   `Dispose()` is called, because the value is not owned by this scope."
//
// ROOT CAUSE. `MethodOwnership::usingVarInitIsRecognizedBorrow`'s `CallExpr`
// branch (src/ownership/Ownership.cpp) positively recognizes ANY directly-
// resolved method whose every `return` is a bare `return this;` as a
// borrow-pin source — correct IN GENERAL per §28.6 "Methods returning
// `this`" (an ordinary receiver-returning method really does hand back a
// borrow, nothing is allocated). A self-returning `GetEnumerator(): IEnumerator
// <int> return this; end GetEnumerator;` (the #2384 idiom — a class
// implementing BOTH `IEnumerable<T>` and `IEnumerator<T>`) structurally
// matches this same shape, so the recognizer wrongly pinned it too. But
// LlvmBackend_Calls2.cpp's manual-`.GetEnumerator()` lowering (the #2388/
// #2440 fix site, `wrapUserClassAsIEnumerable`) does NOT hand the caller the
// raw receiver for this call shape: it always wraps the result in a FRESH,
// calloc'd 32-byte adapter struct (`{ptr vtable, ptr enumerator, ptr source,
// i1 ownsSrc}`, #2424) that the calling scope alone names — a fact only
// codegen knows, invisible to the ownership pass's static, body-shape-only
// scan. `registerUsingDisposal` (LlvmBackend_Setup.cpp) honored the
// (wrongly-set) `usingVarSourceIsBorrow` flag and skipped registering any
// disposal for the wrapper at all: an UNCONDITIONAL 32-byte leak, identical
// for both a NAMED receiver (`s.GetEnumerator()`) and a bare TEMPORARY
// receiver (`new SelfIterRes(3).GetEnumerator()`).
//
// THE FIX (Ownership.cpp, `usingVarInitIsRecognizedBorrow`'s `CallExpr`
// branch): a negative carve, gated on the EXACT same structural shape
// codegen's own `resultIsEnumeratorSentinel` check uses (a zero-arg call
// named `GetEnumerator` whose sema-inferred result type is the builtin,
// declaration-less `IEnumerator<T>`/`IOwningEnumerator<T>` sentinel) —
// mirroring the async-method exclusion ("F1") already present in the same
// function for the analogous Task-family-mint gap. Per §29.1's own prose,
// EVERY manual `.GetEnumerator()` call through this codegen path is an owned
// production the caller must dispose, regardless of the concrete
// implementation's body shape — so the recognizer must never reach the
// body-shape scan for this call shape at all. The wrapper's own `Dispose()`
// (hardened by #2389's alias guard and #2424/#2440's `ownsSrc` runtime flag)
// already resolves the self-returning alias correctly, so registering its
// disposal here is sound, not a double-free — confirmed below under
// runAotChecked (ASan+LSan) for both receiver shapes.
//
// The two-class idiom (`GetEnumerator()` returning a distinct, freshly-`new`'d
// enumerator object, e.g. `IntRange`/`IntRangeEnum`) is UNAFFECTED: its body
// fails the pre-existing `methodReturnsOnlyReceiverSourcedBorrows` structural
// scan on its own (a `new` producer is not receiver-sourced), so it was never
// wrongly recognized as a borrow to begin with — this fix's carve is a no-op
// there (see `two_class_idiom_manual_getenumerator_unaffected` below).
//
// FAILS WITHOUT / PASSES WITH: `named_receiver_disposed_exactly_once` and
// `temporary_receiver_disposed_exactly_once` reproduce the issue's own repro
// verbatim (via the `Res`/`SelfIterRes` live-instance-counter idiom shared
// with iter_selfreturning_wrap_2384_test.cpp and
// iter_temp_source_reclaim_2424_test.cpp) — mechanically confirmed during
// development: reverting this fix (scratch-copy `git checkout` of
// Ownership.cpp) reintroduces the LSan-detected 32-byte leak at exactly the
// `using var e := s.GetEnumerator();` site, forcing `runAotChecked` to return
// `-999` on both tests; rebuilding with the fix in place is required for
// either to pass.

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
// "correct value" in one assertion — exactly-once disposal via a live-object
// counter, not merely "doesn't crash".
int runLeakChecked(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// The issue's own repro classes verbatim: a self-returning class (#2384)
// implementing BOTH `IEnumerable<int>` and `IEnumerator<int>` with an OWNED
// FIELD tracked via a static live-instance counter (`Res.n`), so exact-once
// disposal is positively observable (a no-op destructor could not expose a
// double-dispose).
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
}  // namespace

// THE ISSUE'S EXACT REPRO, NAMED RECEIVER: `using var s := new SelfIterRes
// (3); using var e := s.GetEnumerator();`. Pre-fix: `e`'s wrapper leaks
// unconditionally (32 bytes, LSan); `Res.n` still reaches 0 because `s`'s own
// disposal correctly runs (this fix does not touch that path). Post-fix: `e`
// is ALSO disposed — via the wrapper's own alias-guarded Dispose (#2389),
// which recognizes `enumPtr == srcObj` and does not re-run `s`'s destructor a
// second time — so `Res.n` still reaches exactly 0, and the leak is closed.
BEZEL_TEST(iter_selfreturning_manual_getenumerator_leak_2443, named_receiver_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var s := new SelfIterRes(3); "
        "      using var e := s.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// THE ISSUE'S EXACT REPRO, TEMPORARY RECEIVER: `using var e := new
// SelfIterRes(3).GetEnumerator();` — no other name anywhere binds the source.
// The issue confirms this shape "fails identically" to the named-receiver
// case. Here `e`'s wrapper is the SOLE disposer of BOTH the wrapper and the
// aliased source object (`ownsSrc=true`, #2424): pre-fix, NEITHER was ever
// disposed (the whole wrapper leaked, taking the retained source with it);
// post-fix, `Res.n` reaches exactly 0 via the wrapper's own single dispatch.
BEZEL_TEST(iter_selfreturning_manual_getenumerator_leak_2443, temporary_receiver_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var e := new SelfIterRes(3).GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// ADVERSARIAL: a full manual drain (repeated MoveNext/Current) over the
// self-returning temporary receiver — exercises the wrapper's dispatch
// repeatedly before disposal, not just a single step.
BEZEL_TEST(iter_selfreturning_manual_getenumerator_leak_2443, temporary_receiver_full_drain_disposed_once) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; var guard := 0; "
        "    do "
        "      using var e := new SelfIterRes(4).GetEnumerator(); "
        "      var more := e.MoveNext(); "
        "      while more and guard < 100 do "
        "        sum := sum + e.Current; "
        "        more := e.MoveNext(); "
        "        guard := guard + 1; "
        "      end; "
        "    end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return sum; "  // 1+2+3+4 = 10
        "  end Main; "
        "end C;"), 10);
}

// REGRESSION GUARD (must NOT be affected by this fix): the TWO-CLASS idiom's
// manual `.GetEnumerator()` (§2388/#2424) over a NAMED receiver — a genuinely
// fresh, distinct enumerator object every call — must still dispose exactly
// once and must not be double-freed by this carve (its body already fails
// the receiver-sourced-borrow structural scan on its own, so the fix's new
// `isManualGetEnumeratorWrap` gate is unreachable for it in a way that would
// change behavior; this test proves that empirically, not just by code
// inspection).
BEZEL_TEST(iter_selfreturning_manual_getenumerator_leak_2443, two_class_idiom_manual_getenumerator_unaffected) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 3); "
        "    using var e := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// Value-correctness sanity (non-ASan) for the issue's exact repro shape — a
// plain regression guard independent of the sanitizer harness.
BEZEL_TEST(iter_selfreturning_manual_getenumerator_leak_2443, values_correct_non_asan) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSelfIterRes +
        "class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    do "
        "      using var s := new SelfIterRes(3); "
        "      using var e := s.GetEnumerator(); "
        "      if e.MoveNext() then a := e.Current; end; "
        "    end; "
        "    return a; "  // 1
        "  end Main; "
        "end C;"), 1);
}
