// fix #2389 (§28 "Resource Management" preamble — "Class instances are
// heap-allocated by `new` and must be disposed exactly once" — / §28.2 "The
// `IDisposable` contract" / §29.1 "The protocol" / §29.11 "Multiple
// enumeration", docs v0.235):
//
//   §29.1: "`GetEnumerator()` returns `owned IEnumerator<T>`: the caller
//   takes ownership of the enumerator and must dispose it."
//
//   §29.11: "Each `GetEnumerator()` call produces an independent
//   enumerator."
//
// ROOT CAUSE (`wrapUserClassAsIEnumerable`, LlvmBackend_Assign.cpp,
// #552/#2384's wrapper that normalizes a user class's `GetEnumerator()`
// result into the §29.11 iterator-vtable format). Its generated
// `wrapDisposeFn` disposed the retained inner enumerator
// (`enumPtr`, slot 1) by calling `enumDisposeFn` — `disposerFns_[enumTs]`,
// the flag-guarded PUBLIC `Dispose()` (§28.2: idempotent, dispatch only) —
// then freed only the WRAPPER struct itself (`self`). Per
// `emitDynamicDispose`'s own documented contract ("This does NOT free the
// object's heap block", LlvmBackend_Disposal.cpp) that dispatch never
// reclaims memory; every OTHER terminal disposal site in the codebase pairs
// it with a reclaim (`emitDisposeAndReclaim`). This wrapper was the one
// terminal site that skipped the reclaim half — so every enumerator minted
// through it (a fresh one per `.GetEnumerator()` call, §29.11) leaked its
// calloc'd block unconditionally. Confirmed under LSan on the issue's own
// two-class-idiom repro (`IntRange`/`IntRangeEnum`, no self-return
// involved): "Direct leak of 48 byte(s) in 2 object(s) ... in
// bz$M.IntRange$GetEnumerator" (one per `.GetEnumerator()` call).
//
// (NOTE ON TRIVIAL-DISPOSE: `IntRangeEnum` below has no destructor and no
// owned fields, so it is a trivial-dispose class — §28.9: "Memory for
// trivial-dispose instances is reclaimed by an implementation-defined
// mechanism ... that runs no user code." The spec leaves the RECLAIM
// mechanism implementation-defined for such classes; THIS implementation's
// chosen mechanism is an explicit calloc/free pairing (no GC — the #380
// design commitment threaded through every other disposal site,
// LlvmBackend_Disposal.cpp's `emitDisposeAndReclaim`), and having chosen
// that mechanism, every calloc this wrapper performs must eventually be
// paired with a free to actually deliver on it — which is what this fix
// restores for the one site that was missing it.)
//
// THE FIX (`wrapDisposeFn`, LlvmBackend_Assign.cpp): after dispatching
// `enumDisposeFn(enumPtr)`, free `enumPtr`'s block too — UNLESS it aliases
// the retained source object at slot 2 (`srcObj`), checked with a RUNTIME
// pointer-equality guard. That alias arises only in the single-object
// self-returning idiom (#2384: a class implementing BOTH `IEnumerable<T>`
// and `IEnumerator<T>` whose own `GetEnumerator()` returns `this` — there
// `enumPtr == srcObj` by identity, and `srcObj` is independently owned and
// reclaimed by ITS OWN scope-end/using-var disposal, so freeing it again
// here would double-free). The ordinary two-class idiom never aliases, so
// the guard is a runtime no-op there and the leak closes unconditionally.
//
// FAILS WITHOUT / PASSES WITH: `two_manual_calls_no_leak` below is caught
// by LeakSanitizer (via `runAotChecked`, which flips `.ran` to false on any
// sanitizer report) without the fix, and runs clean with it — mechanically
// confirmed during development (scratch-copy revert of
// LlvmBackend_Assign.cpp reproduces the LSan report; reapplying the fix and
// rebuilding clears it). `self_returning_no_double_free` pins the OTHER
// direction: the alias guard must not itself introduce a double-free
// (checked via ASan, `runAotCheckedLenient`, LSan off so only crash/UAF/
// double-free fails the test, not the separate pre-existing leak below).
//
// KNOWN, SEPARATE, PRE-EXISTING LEAK (discovered during this lane's
// verify-first pass, filed as a new issue, NOT fixed here): the
// `IEnumerable<int>`-typed local-decl coercion site
// (`wrapUserClassAsIEnumerable` invoked from a `var s: IEnumerable<int> :=
// new IntRange(...);` declaration, LlvmBackend_Setup.cpp) retains the
// ORIGINAL freshly-`new`'d source object (`srcObj`, slot 2) with no other
// name/owner and never reclaims it — a leak of the coerced-away temporary
// itself, independent of (and unfixed by) this commit's `enumPtr` fix. This
// is NOT the defect #2389 describes (#2389 is specifically about `enumPtr`,
// per the issue's own root-cause text) — confirmed via isolated probe: a
// `var s: IEnumerable<int> := new IntRange(1, 5);` local with NO
// `.GetEnumerator()` call anywhere still leaks 24 bytes, both before and
// after this fix, identically. Every test below therefore uses a
// CONCRETE-typed named source (`using var s := new IntRange(...);` — the
// #2388 manual-call shape, which independently, separately disposes `s`
// itself via the ordinary class disposal path) so this file's LEAK
// assertions are not confounded by that separate, still-open defect. This
// also means these leak-checking tests mechanically require #2388's fix
// (LlvmBackend_Calls2.cpp) to be present too — without it, `s.GetEnumerator()`
// on a concrete-typed receiver crashes before the leak-check point is ever
// reached, so `runAotChecked` still correctly reports `.ran == false`
// either way, but for the crash rather than the leak. Both fixes ship
// together in this lane; if a future gate drops one it must drop both for
// this file to stay meaningful.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// Leak-checked run: -999 if a sanitizer report fired (leak/UAF/double-free/
// crash) OR the program otherwise failed to run cleanly; the exit code
// otherwise. A passing BEZEL_EXPECT_EQ against the expected value therefore
// proves BOTH "no sanitizer report" and "correct behavior" in one assertion.
int runLeakChecked(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// ASan-only (LSan off): fails on crash/UAF/double-free, tolerant of a leak.
int runCrashCheckedOnly(const std::string& prog) {
    JitResult r = runAotCheckedLenient(prog);
    return r.ran ? r.exitCode : -999;
}

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

// Baseline: a SINGLE manual `.GetEnumerator()` call on a concrete-typed
// named source must not leak its inner enumerator.
BEZEL_TEST(iter_wrapdispose_reclaim_2389, single_manual_call_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 3); "
        "    using var e := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "
        "  end Main; "
        "end C;"), 1);
}

// THE ISSUE'S EXACT REPRO SHAPE (adapted to a concrete-typed `s` so the
// separate src-retention leak documented above cannot confound this
// assertion): TWO manual `.GetEnumerator()` calls, each minting its own
// inner enumerator (§29.11 independence) — both must be reclaimed, not just
// disposed.
BEZEL_TEST(iter_wrapdispose_reclaim_2389, two_manual_calls_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
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
        "    return a * 10 + b; "
        "  end Main; "
        "end C;"), 11);
}

// Adversarial: THREE nested, simultaneously-live manual enumerators on the
// same named source (each disposed at its own scope end, LIFO) — multiple
// enumerators live at once, all reclaimed, none double-freed.
BEZEL_TEST(iter_wrapdispose_reclaim_2389, three_nested_live_enumerators_no_leak) {
    BEZEL_EXPECT_EQ(runLeakChecked(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    using var s := new IntRange(1, 9); "
        "    using var e1 := s.GetEnumerator(); e1.MoveNext(); "
        "    using var e2 := s.GetEnumerator(); e2.MoveNext(); e2.MoveNext(); "
        "    using var e3 := s.GetEnumerator(); e3.MoveNext(); e3.MoveNext(); e3.MoveNext(); "
        "    return e1.Current * 100 + e2.Current * 10 + e3.Current; "  // 1*100+2*10+3 = 123
        "  end Main; "
        "end C;"), 123);
}

// Adversarial (dispose/free ordering — the aliasing case #2389's fix must
// NOT break): a SELF-RETURNING class (`GetEnumerator()` returns `this`) —
// here `enumPtr` and the retained `srcObj` are the SAME object. The fix's
// runtime alias guard must skip freeing it inside the wrapper (the source's
// own separate disposal reclaims it), or this doubles-frees under ASan.
// LSan is OFF here (runAotCheckedLenient) because the wrapper's OWN eager
// `GetEnumerator()` fetch at `s`'s construction retains a temporary that —
// per the SAME pre-existing, separately-filed src-retention gap this file's
// header discusses — is not reclaimed for an `IEnumerable<int>`-typed `s`;
// this test's purpose is exclusively to prove no crash/UAF/double-free from
// the alias guard, not to re-litigate that separate leak.
BEZEL_TEST(iter_wrapdispose_reclaim_2389, self_returning_no_double_free) {
    BEZEL_EXPECT_EQ(runCrashCheckedOnly(
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
        "    var s: IEnumerable<int> := new SelfIter(1, 3); "
        "    using var e := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e.MoveNext() then a := e.Current; end; "
        "    return a; "
        "  end Main; "
        "end C;"), 1);
}

// Regression guard: `for` over a concrete-typed source (lowerIEnumerableFor,
// a completely different codegen path, #2384) is untouched by this fix — a
// plain VALUE-correctness regression guard, `run()` not `runLeakChecked()`.
// (Adversarial-pass discovery, filed separately, NOT fixed here: this exact
// program DOES leak 24 bytes under LSan — `lowerIEnumerableFor`,
// LlvmBackend_Stmts.cpp, disposes only the ENUMERATOR it constructs and
// never the temporary SOURCE object `new IntRange(1, 4)` produces, when the
// `for`-loop's iterable is an unnamed temporary rather than a named local —
// the same family of "retained temporary never reclaimed" defect as the
// src-retention gap noted above, but in a third, different function. Using
// `runLeakChecked` here would make this test fail regardless of this
// commit's fix, for a reason unrelated to `wrapDisposeFn`.)
BEZEL_TEST(iter_wrapdispose_reclaim_2389, for_loop_over_concrete_source_value_unaffected) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntRangeTwoClass +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in new IntRange(1, 4) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;"), 10);
}
