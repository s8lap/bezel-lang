// fix #2384 / §29.1/§29.11 (v0.235): `wrapUserClassAsIEnumerable`
// (LlvmBackend_Assign.cpp) and `lowerIEnumerableFor` (LlvmBackend_Stmts.cpp)
// fail to handle a class that implements BOTH `IEnumerable<T>` and
// `IEnumerator<T>` and returns `this` from `GetEnumerator()` — the
// single-object self-returning idiom.
//
// ROOT CAUSE. Both functions resolve `GetEnumerator()`'s concrete enumerator
// class via a fallback scan, triggered whenever the method's declared return
// type is the bare builtin `IEnumerator<T>` — the ONLY spelling admissible
// for a class satisfying the `IEnumerable<T>` interface's
// `method GetEnumerator(): owned IEnumerator<T>;` when the runtime object is
// not itself the returned instance's exact declared type (§11.2/§12.1
// "Member matching": a member's return type must be "exactly equal ... under
// the interface's substitution"). The scan unconditionally excluded
// `cand == srcTs` / `candidate == ts`, so a self-implementing class was NEVER
// found:
//   * `wrapUserClassAsIEnumerable`: `enumTs` stayed null, and the function
//     returned the raw, UNWRAPPED class instance (`if (!enumTs) return src;`).
//     Any iterator-vtable dispatch on the resulting `IEnumerable<T>`-typed
//     value then read past the object's small type-descriptor header — a
//     confirmed ASan global-buffer-overflow (this file's
//     `manual_getenumerator_...` tests, the issue's own repro).
//   * `lowerIEnumerableFor` (the `for var x in <concrete-class-typed source>
//     do ... end` desugaring): the same exclusion meant a self-returning
//     class used DIRECTLY as a for-in source (no interface-typed
//     intermediate) fell through every for-handler to the generic "not yet
//     implemented: codegen for this for statement" diagnostic — a HARD
//     COMPILE FAILURE, confirmed via probe as part of this issue's
//     VERIFY-FIRST matrix (`for-in vs. manual .GetEnumerator()`, both
//     asked about explicitly in the brief).
//
// SPEC READING: is a self-returning single object even LEGAL under §29.11?
// §29.1: "GetEnumerator() returns owned IEnumerator<T>: the caller takes
// ownership of the enumerator and must dispose it" and a call "produces a
// fresh enumerator positioned just before the first element." §29.11: "Each
// GetEnumerator() call produces an independent enumerator." YES, legal —
// for two reasons: (1) precedent — #2356's GetAsyncEnumerator fix explicitly
// preserves a defense-in-depth identity guard "for arbitrary user
// IAsyncEnumerable<T> implementations that legally return `this` from a
// hand-written GetAsyncEnumerator() — a case the compiler cannot rule out",
// i.e. the project has already ruled a HAND-WRITTEN self-return legal (as
// opposed to #2356's own subject: a COMPILER-GENERATED async iterator's
// silent receiver-identity reuse, which involved no user choice and was
// ruled non-compliant). (2) §29.11's "independent enumerator" is satisfied
// at the WRAPPER level without requiring the underlying STATE be
// independent: `wrapGetEnumFn` mints a genuinely fresh `calloc`'d adapter
// struct on every call (the #943 mechanism) and re-invokes the source's own
// `GetEnumerator()` each time; when that body happens to return `this`, the
// wrapper is fresh but its cursor necessarily aliases the shared object — a
// consequence of the CLASS's own implementation choice, not a compiler-
// introduced violation. Nothing in §29.1/§29.11 requires a second call to be
// illegal, or requires the independence to extend to the implementation's
// internal state — only that a fresh call happens, which it does. So: WRAP,
// don't reject (the alternative direction the brief raised).
//
// DISPOSAL SAFETY (adversarial self-pass finding). Lifting the exclusion in
// `lowerIEnumerableFor` alone would have introduced a NEW heap-use-after-free:
// its enumerator-scope disposal RECLAIMS (physically frees) the object at
// loop exit, which is correct for the two-class idiom (a genuinely distinct,
// solely-owned allocation) but wrong for self-return over a NAMED source —
// `enumPtr` and `srcPtr` are the SAME object, and the named source's own
// later scope-end disposal reclaims it AGAIN through a different alloca this
// scope's null-guard cannot see nulled. Fixed with a runtime identity guard
// mirroring `lowerAwaitFor`'s pre-existing `afor.enown` guard
// (#1268/#2356) — see the fix commit for the full analysis.
//
// MECHANICAL PROOF: every test below reproduces the pre-fix symptom (ASan
// global-buffer-overflow, "not yet implemented" compile error, or heap-use-
// after-free) when LlvmBackend_Assign.cpp / LlvmBackend_Stmts.cpp are
// reverted via scratch-copy `git checkout` (confirmed during development;
// see the fix commit message for the exact matrix). All tests assert the
// ITERATED VALUES, not just pass/fail — this family has produced silent
// wrong-value bugs.
//
// ORTHOGONAL, PRE-EXISTING BUGS DISCOVERED (filed, NOT fixed here — neither
// is specific to self-return, both reproduce identically on the unpatched
// tree via the ALREADY-WORKING two-class idiom):
//   * #2388 — a manual `.GetEnumerator()` call on a CONCRETE-class-typed
//     (not `IEnumerable<T>`-typed) source SEGVs in itable-lookup machinery
//     unrelated to either function this fix touches. Tests below therefore
//     never take that shape (self-returning OR two-class) — every manual
//     `.GetEnumerator()` test here uses an `IEnumerable<T>`-typed named
//     local, exactly like the issue's own repro and #2376's own test file.
//   * #2389 — `wrapUserClassAsIEnumerable`'s `wrapDisposeFn` never
//     physically reclaims the retained inner enumerator object (a leak,
//     present for the two-class idiom too). Tests that exercise the
//     wrapper path (an `IEnumerable<T>`-typed manual-call source) therefore
//     use the non-leak-checking `run()` helper, exactly as #2376's own
//     `user_class_wrapped_named_local_for_in_twice_unaffected` test does for
//     the identical reason; tests that exercise the leak-FREE concrete-typed
//     `for-in` path (this fix's `lowerIEnumerableFor` piece) use the
//     leak-checking `runChecked()` helper to positively prove no leak / no
//     double-free.

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
// A single class implementing BOTH IEnumerable<int> and IEnumerator<int>,
// returning `this` from GetEnumerator() — the issue's exact shape. `cur` is
// NOT reset by GetEnumerator(), so re-enumeration continues from wherever
// the shared cursor was left — the class's own implementation choice, not a
// compiler artifact (documented explicitly in several tests below).
const char* kSelfIter =
    "class SelfIter: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfIter; ";
// Same shape, but with a real OWNED CLASS FIELD (`Res`, tracked via a static
// live-instance counter, mirroring iter_named_getenumerator_reenum_2376_
// test.cpp's `kCell`) so disposal actually does observable work — a trivial
// no-op destructor (as SelfIter above has) cannot expose a double-free, and
// a plain ASan/LSan pass alone cannot distinguish "disposed once" from
// "never disposed" as precisely as the counter can.
const char* kSelfIterWithOwnedField =
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

// §29.1/§29.11: THE ISSUE'S EXACT REPRO. A single class implementing both
// interfaces, assigned to a NAMED `IEnumerable<int>` local, enumerated
// through TWO manual `.GetEnumerator()` calls. Before the fix: ASan
// global-buffer-overflow (wrapUserClassAsIEnumerable returned the raw,
// unwrapped instance; any vtable-slot dispatch on it read past the 16-byte
// type descriptor). After the fix: no crash, and the ALIASING result the
// spec reading above predicts — e2 continues e1's already-advanced cursor
// (a=1, b=2), because the wrapper's clone re-invokes SelfIter's OWN
// GetEnumerator(), which returns the SAME object every time.
BEZEL_TEST(iter_selfreturning_wrap_2384, issue_exact_repro_no_crash_aliased_result) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSelfIter +
        "class C "
        "  public static method Main(): int "
        "    var s: IEnumerable<int> := new SelfIter(3); "
        "    using var e1 := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := s.GetEnumerator(); "
        "    var b := 0; "
        "    if e2.MoveNext() then b := e2.Current; end; "
        "    return a * 10 + b; "  // 1*10+2 = 12 (aliased cursor)
        "  end Main; end C;"), 12);
}

// §29.1/§29.11: two ENUMERATORS alive AT THE SAME TIME (interleaved
// MoveNext/Current calls, not sequential drains) — the direct test of
// "concurrent enumeration of the same instance". Confirms the aliasing is
// exactly a SHARED CURSOR: interleaved advances observe each other's
// progress, consistent with both wrapper instances re-invoking the SAME
// underlying self-returned object's GetEnumerator/MoveNext/Current.
BEZEL_TEST(iter_selfreturning_wrap_2384, two_concurrent_enumerators_share_cursor) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSelfIter +
        "class C "
        "  public static method Main(): int "
        "    var s: IEnumerable<int> := new SelfIter(5); "
        "    using var e1 := s.GetEnumerator(); "
        "    using var e2 := s.GetEnumerator(); "
        "    e1.MoveNext(); var v1 := e1.Current; "      // cur=1, v1=1
        "    e2.MoveNext(); var v2 := e2.Current; "      // cur=2, v2=2 (shared!)
        "    e1.MoveNext(); var v3 := e1.Current; "      // cur=3, v3=3
        "    return v1 * 100 + v2 * 10 + v3; "            // 1*100+2*10+3 = 123
        "  end Main; end C;"), 123);
}

// §29.2/§29.11: for-in over an `IEnumerable<int>`-TYPED named local wrapping
// a self-returning class — the interface-typed for-in path
// (lowerIEnumerableBuiltinFor -> wrapUserClassAsIEnumerable's vtable slot 4).
// Before the fix: ASan global-buffer-overflow (same root cause as above,
// reached one call earlier since #2376 made `for`'s own GetEnumerator() call
// dispatch through the vtable unconditionally). After: correct sum.
BEZEL_TEST(iter_selfreturning_wrap_2384, forin_over_interface_typed_local_sums_correctly) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kSelfIter +
        "class C "
        "  public static method Main(): int "
        "    using var s: IEnumerable<int> := new SelfIter(3); "
        "    var sum := 0; "
        "    for var x in s do sum := sum + x; end; "
        "    return sum; "  // 1+2+3 = 6
        "  end Main; end C;"), 6);
}

// §29.2/§29.11: for-in DIRECTLY over a CONCRETE-class-typed local (no
// interface-typed intermediate at all) — the `lowerIEnumerableFor` path this
// fix's second piece touches. Before the fix: hard COMPILE ERROR ("not yet
// implemented: codegen for this for statement") — this case never reached
// codegen at all before. After: no crash, no leak (runChecked — this path
// never aliases a wrapper struct, so it is leak-free unlike the wrapper
// path), correct sum.
BEZEL_TEST(iter_selfreturning_wrap_2384, forin_over_concrete_typed_local_sums_correctly) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kSelfIter +
        "class C "
        "  public static method Main(): int "
        "    using var s := new SelfIter(3); "
        "    var sum := 0; "
        "    for var x in s do sum := sum + x; end; "
        "    return sum; "  // 1+2+3 = 6
        "  end Main; end C;"), 6);
}

// §28.2/§29.1 DISPOSAL SAFETY: two SEQUENTIAL for-in loops over the SAME
// named concrete-typed self-returning source. This is the case the
// adversarial self-pass caught: without the `lowerIEnumerableFor` identity
// guard, the FIRST loop's own enumerator-scope disposal reclaims the shared
// object at loop exit; the SECOND loop's `GetEnumerator()` call (which
// returns the SAME, now-freed, `this`) and the source's own later scope-end
// disposal both then touch freed memory — confirmed heap-use-after-free
// under ASan without the guard. With the guard: the first loop's disposal is
// skipped (the named source `s` independently owns the object and disposes
// it once, at ITS OWN scope end) — no crash, no leak (runChecked). The
// VALUES also document the aliasing consequence precisely: the second loop
// does NOT independently replay (unlike the true-clone #2376/#943 cases) —
// it continues from where the shared cursor was left by the first loop's
// exhaustion (cur ends at hi+2=7 after the first loop's final failed
// MoveNext), so the second loop's very first MoveNext (cur:=8) already
// exceeds hi=5 and the loop body never runs. This is the self-returning
// class's OWN choice (GetEnumerator() does not reset `cur`), not a compiler
// defect — contrast with `forin_twoclass_...` below, where the two-class
// idiom (a fresh IntRangeEnum per GetEnumerator() call) DOES independently
// replay.
BEZEL_TEST(iter_selfreturning_wrap_2384,
           forin_twice_over_named_concrete_source_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kSelfIter +
        "class C "
        "  public static method Main(): int "
        "    using var s := new SelfIter(5); "
        "    var a := 0; "
        "    for var x in s do a := a + x; end; "
        "    var b := 0; "
        "    for var x in s do b := b + x; end; "
        "    return a * 100 + b; "  // a=1+2+3+4+5=15, b=0 (already exhausted)
        "  end Main; end C;"), 1500 % 256);
}

// §28.2/§28.5/§29.1 owned-element disposal, checked under ASan+LSan
// (runChecked = generated-code ASan AND leak detection) AND a static
// live-instance counter (belt-and-suspenders): the self-returning object
// owns a real class field (`Res`). A double-dispose of the shared
// self-returned object would double-free `Res` — the concrete failure mode
// the #2384 disposal-safety analysis is about, made observable (a trivial
// no-op destructor, as `SelfIter` above has, cannot expose a double-free at
// all; this class can, and `Res.n` catches BOTH a double-free — it would go
// negative or ASan would abort first — and a leak — it would stay positive).
// No crash, no leak, `Res.n == 0`: disposed exactly once, via the named
// source `s`'s own scope-end disposal (the for-loop's own registration is
// skipped by the identity guard, exactly as in the plain-int test above).
BEZEL_TEST(iter_selfreturning_wrap_2384,
           forin_owned_field_disposed_exactly_once_under_asan) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kSelfIterWithOwnedField +
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var s := new SelfIterRes(3); "
        "      for var x in s do sum := sum + x; end; "
        "    end; "
        "    if Res.n <> 0 then return 99; end; "
        "    return sum; "  // 1+2+3 = 6
        "  end Main; end C;"), 6);
}

// §29.11 no-regression: the ESTABLISHED two-class idiom (IntRange /
// IntRangeEnum — a genuinely distinct enumerator allocation per
// GetEnumerator() call) via for-in DIRECTLY over a concrete-class-typed
// named source, run TWICE. This is the control this fix's `enumTs == ts`
// structural check must NOT fire for (`enumTs` = IntRangeEnum, `ts` =
// IntRange — never equal), so the identity guard added for disposal safety
// must never activate here either: the ordinary distinct-object disposal
// runs for BOTH loops, and — unlike the self-returning case above — the
// second loop DOES independently replay (a fresh IntRangeEnum per call),
// proving this fix does not regress the working idiom's re-iterability.
BEZEL_TEST(iter_selfreturning_wrap_2384,
           forin_twoclass_idiom_over_concrete_source_twice_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
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
        "    using var r := new IntRange(1, 3); "
        "    var s1 := 0; for var x in r do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in r do s2 := s2 + x; end; "
        "    if s1 <> 6 then return 1; end; "
        "    if s2 <> 6 then return 2; end; "  // independent replay: unaffected by this fix
        "    return 0; "
        "  end Main; end C;"), 0);
}

// §29.3: `GetEnumerator()` declared with the CONCRETE return type
// (`GetEnumerator(): SelfIter return this;`) instead of the bare interface —
// confirmed via probe to ALREADY work before this fix (it resolves via the
// direct `enumTyRef->declaration` branch, never reaching the fallback scan
// this fix touches at all) — kept here as a permanent regression guard
// documenting that this spelling was never the affected one.
BEZEL_TEST(iter_selfreturning_wrap_2384,
           concrete_declared_return_type_variant_unaffected_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class SelfIter2: IEnumerable<int>, IEnumerator<int> "
        "  private cur: int := 0; private hi: int; "
        "  public constructor(hi: int) this.hi := hi; end constructor; "
        "  public method GetEnumerator(): SelfIter2 return this; end GetEnumerator; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end SelfIter2; "
        "class C "
        "  public static method Main(): int "
        "    var s: IEnumerable<int> := new SelfIter2(3); "
        "    using var e1 := s.GetEnumerator(); "
        "    var a := 0; "
        "    if e1.MoveNext() then a := e1.Current; end; "
        "    using var e2 := s.GetEnumerator(); "
        "    var b := 0; "
        "    if e2.MoveNext() then b := e2.Current; end; "
        "    return a * 10 + b; "  // 12: same aliasing result as the bare-interface spelling
        "  end Main; end C;"), 12);
}
