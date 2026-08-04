// Codegen — fix #2556 (spec §29.2 / §29.7, v0.236): `continue` in the body of
// a vtable-protocol `for var x in <IEnumerable<T>>` over a LOCAL (or a user
// class implementing IEnumerable<T>) must NOT null the enumerator's slot
// mid-loop. Before the fix, `loopStack_` was pushed with `scopeDepth =
// outerDepth` (the depth BEFORE the enumerator's own disposal scope was
// pushed), so on `continue` the disposal walk reached the enumerator's slot,
// `iterEnumDispose` nulled it, and the very next condBB re-loaded the slot
// (`Load(enumSlot)`) and dereferenced NULL → SEGV at address 0x000000000000.
//
// The pre-existing `lowerIEnumerableForSortedSet` helper (#2553) already used
// `disposeStack_.size()` (AFTER push) and was the only branch without this
// latent bug — it was the first vtable-protocol for-in to exercise `continue`
// (sorted_set_forin_string_with_break_continue_spec_4_18). #2556 brings the
// two older `lowerIEnumerableFor` / `lowerIEnumerableBuiltinFor` sites in
// line with the helper.
//
// The bug is unreachable from the prior test suite because the array-backed
// `for` path (the `for var x in xs` over a concrete `Array<T>`) does not
// null its slots on `continue` — only the vtable-protocol path over the
// BUILTIN `IEnumerable<T>` (the `lowerIEnumerableBuiltinFor` vtable-dispatch
// branch) and over a user class implementing `IEnumerable<T>` (the
// `lowerIEnumerableFor` `emitMethodCall` path) carry the bug.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Stmts.cpp): `loopStack_.push_back({
// condBB, endBB, outerDepth })` in `lowerIEnumerableBuiltinFor` (~line 6533)
// and `lowerIEnumerableFor` (~line 6925), with `outerDepth` recorded BEFORE
// the enumerator's own `disposeStack_.emplace_back()`. On `continue`,
// `emitLoopDisposals(loopStack_.back().scopeDepth)` walked
// `disposeStack_[scopeDepth .. back]` — but `scopeDepth` was the depth
// ABOVE the enumerator's scope, so the walk INCLUDED the enumerator's slot,
// `iterEnumDispose` set it to NULL, and the next `condBB` re-load → SEGV.
//
// FIX: change both sites to push `disposeStack_.size()` (the depth AFTER
// the enumerator scope was just pushed). The wrapper/enumerator slots are
// loop-scope resources that must outlive every `continue`; `break` and
// normal exhaustion still dispose them via the endBB path
// (`emitDisposals(disposeStack_.back())`). This matches the existing
// `lowerIEnumerableForSortedSet` (#2553) and the existing
// `lowerLinkedListFor` iter-var scope shape (which already used the
// post-push depth).
//
// Fail-without / pass-with: every test below is observed SEGV (PC in main,
// rax=0) on the unfixed compiler (a genuine address-0 dereference the AOT
// process-isolation harness turns into a non-zero exit). On the fixed
// compiler every program returns the asserted value with no ASan error.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT + ASan oracle (the SEGV is
                                    //         an AOT process-exit, not an in-
                                    //         process crash, so runAot
                                    //         captures the symptoms cleanly)
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A SEGV under AOT shows as a non-zero exit and r.ran==true (the kernel
// propagated the signal into a real exit code we can assert on, distinct
// from any legal value the program would return).
int run(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return -999;
    return r.exitCode;
}

// A user-class enumerator shared by the `lowerIEnumerableFor` cases —
// mirrors the iface_ienumerable_for_1776 preamble so we exercise the
// `emitMethodCall` path (NOT the vtable-dispatch builtin path) for the
// user-class source.
const char* kUserClass =
    "module M; "
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "class IntRange "
    "  protected lo: int; protected hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
    "end IntRange; ";

}  // namespace

// =============================================================================
// §29.2 / §29.7 / fix #2556 (v0.236): vtable-protocol for-in over a
// BUILTIN `IEnumerable<T>` local, with `continue` in the body.
// lowerIEnumerableBuiltinFor's vtable-dispatch branch.
// =============================================================================

// Issue's canonical repro. `Array<int>` literal bound to an `IEnumerable<int>`
// local, then iterated with a `continue` in the body. Pre-fix: condBB
// re-loads a NULL enumSlot on the post-`continue` MoveNext and dereferences
// it → SEGV (address 0). Post-fix: the wrapper/enumerator slot survives
// the continue, the next MoveNext reads a live wrapper, the loop
// terminates cleanly. 1+2+4+5 = 12 (3 skipped by continue).
BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           builtin_ienumerable_local_continue_does_not_segv_spec_29_7) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  var seq: IEnumerable<int> := a; "
        "  var sum: int := 0; "
        "  for var v in seq do "
        "    if v = 3 then continue; end; "
        "    sum := sum + v; "
        "  end; "
        "  return sum; "                   // 12
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 12);
}

// REGRESSION GUARD: a `break` out of the same vtable-protocol for-in must
// still dispose the enumerator and stop iterating at the break point.
// Pre-fix did dispose the enumerator (the wrong-but-sound code path on
// `break`); post-fix must still dispose it (the endBB path runs
// `emitDisposals(disposeStack_.back())` exactly once on `break`). 1+2+3 = 6
// (break at 4).
BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           builtin_ienumerable_local_break_still_works_spec_29_7) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  var seq: IEnumerable<int> := a; "
        "  var sum: int := 0; "
        "  for var v in seq do "
        "    if v >= 4 then break; end; "
        "    sum := sum + v; "
        "  end; "
        "  return sum; "                   // 6
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// REGRESSION GUARD: normal exhaustion (no break, no continue) must still
// dispose the enumerator exactly once at endBB. 1+2+3+4+5 = 15.
BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           builtin_ienumerable_local_normal_exhaustion_still_works_spec_29_7) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3, 4, 5]; "
        "  var seq: IEnumerable<int> := a; "
        "  var sum: int := 0; "
        "  for var v in seq do sum := sum + v; end; "
        "  return sum; "                   // 15
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 15);
}

// NESTED: continue in the inner for-in over a second `IEnumerable<int>`
// local must not double-destroy either enumerator across the
// outer-iteration boundary. (1+2+3) * (10+30) = 6 * 40 = 240 (inner
// continue at 20; both inner and outer iterators are re-used on each
// outer iteration).
BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           nested_builtin_ienumerable_locals_continue_spec_29_7) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  var b: Array<int> := [10, 20, 30]; "
        "  var s1: IEnumerable<int> := a; "
        "  var s2: IEnumerable<int> := b; "
        "  var sum: int := 0; "
        "  for var x in s1 do "
        "    for var y in s2 do "
        "      if y = 20 then continue; end; "
        "      sum := sum + x * y; "
        "    end; "
        "  end; "
        "  return sum; "                   // 240
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 240);
}

// =============================================================================
// §29.2 / §29.7 / fix #2556 (v0.236): user-class for-in with `continue` —
// the `lowerIEnumerableFor` `emitMethodCall` path. Same fix site, same
// shape, but the enumerator is allocated by `GetEnumerator()` and the
// MoveNext/Current calls go through static dispatch on the enumerator
// class. The bug is identical: the enumerator's slot is nulled on
// `continue` and condBB dereferences it.
// =============================================================================

BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           user_class_forin_continue_does_not_segv_spec_29_7) {
    std::string src = std::string(kUserClass) +
        "class C public static method Main(): int "
        "  using var r := new IntRange(1, 5); "
        "  var sum: int := 0; "
        "  for var v in r do "
        "    if v = 3 then continue; end; "
        "    sum := sum + v; "
        "  end; "
        "  return sum; "                   // 1+2+4+5 = 12
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 12);
}

// REGRESSION GUARD: break in the user-class for-in still disposes the
// enumerator exactly once. 1+2+3 = 6.
BEZEL_TEST(codegen_iter_continue_vtable_protocol_2556,
           user_class_forin_break_still_works_spec_29_7) {
    std::string src = std::string(kUserClass) +
        "class C public static method Main(): int "
        "  using var r := new IntRange(1, 100); "
        "  var sum: int := 0; "
        "  for var v in r do "
        "    if v >= 4 then break; end; "
        "    sum := sum + v; "
        "  end; "
        "  return sum; "                   // 1+2+3 = 6
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}
