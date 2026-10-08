// Codegen — issue #4616: a `using var` local whose own type is a BARE TYPE
// PARAMETER — the canonical producer being `OwningStack<T>.Pop()`, declared
// `owned T` — registers NO scope-end disposal inside a monomorphized generic
// body (§28.3 / §14.1 / §14.4 / §3.6, v0.307).
//
// AUTHORITY. §28.3 makes the end of a `using var` binding's scope a disposal
// point unconditionally; stdlib §4.11 declares `OwningStack<T>.Pop(): owned T`
// ("transfers out") and `OwningQueue<T>.Dequeue(): owned T`, so the popped
// element's discharge obligation is the caller's and the `using var` is where
// it is discharged. §3.6 makes an element's category a DERIVED property of its
// type, and §14.4 fixes the owning family's class/interface restriction at the
// INSTANTIATION rather than at the generic declaration — so the classification
// that governs the disposal is the one at the substitution. §14.1's
// monomorphization is what makes `T` concrete at the point this disposal is
// registered.
//
// THE BUG, located by instrumenting the compiler rather than by reading the
// issue. The issue attributes the loss to `classOfLocal`; that is NOT where it
// happens. Measured with a `fprintf` at each arm of `registerUsingDisposal`,
// on the issue's repro and on its non-generic twin:
//
//     GENERIC    [RUD] top classOfLocal=(nil)
//                [RUD] top dyn=0 initTyDecl=(nil) head=T resolvedDecl=0x4031…
//     CONCRETE   [RUD] top classOfLocal=(nil)
//                [RUD] top dyn=1 initTyDecl=0x33cc… head=Res resolvedDecl=…
//
// `classOfLocal` returns null on BOTH sides — it has no arm for a CallExpr
// initializer with no type annotation — so it is not the discriminator. The
// arm that claims the concrete twin is `usingVarOwnsClassObjectDynamically`
// case (b2), the builtin owned-TRANSFER call arm (#642/#643). Its gate reads
// `isClassOrInterfaceRef(ctx_.exprType(*ld->initializer))` and its disposer
// lookup reads `eTy->declaration` — and inside the generic body that sema type
// is still the placeholder `T`, a `Named` TypeRef with a NULL `declaration`.
// (b2) was the ONLY arm of that classifier testing the raw `exprType` with no
// `|| isClassOrInterfaceRef(resolveTypeParamRef(...))` companion: (a1), (a1u),
// (a2), (b0), (b0b), (b0c), (b0d), (b1) and (b1f) all carry one. So the
// predicate answered false, no `Disposal` was pushed, and the `using var`
// silently became a no-op.
//
// THE FIX resolves the result type through the active monomorphization once,
// at the top of (b2) — `resolveTypeParamRef(ctx_.exprType(...))` — and uses
// that one ref for both the category test and the disposer lookup. Same root
// cause and same shape as fix #4596 in `elementPolicyFor`, but a DIFFERENT
// path: #4596 governs a collection's ELEMENT slots, this governs the
// `using var` local's own scope-end disposal. #4596 explicitly measured that
// it does not close this — its repro leaves the collection EMPTY at scope end,
// so no element policy is consulted.
//
// MEASURED on this worktree, `bezelc -o q && ./q` on the issue's repro at 100
// iterations: the generic form returned `Res._n == 0`, the non-generic twin
// `Res._n == 100`. Post-fix both return 100.
//
// ORACLE DISCIPLINE. Every row counts destructor runs on a static counter and
// asserts an EXACT number, so a MISSED disposal and a DOUBLED one are both a
// wrong answer independently of any sanitizer, and every row loops (a one-shot
// leak repro passes vacuously on both compilers — LSan reports only
// *unreachable* allocations and one iteration leaves the object reachable from
// a live stack slot). Counts stay at or below 200 because `runAot`'s
// `exitCode` is `WEXITSTATUS`-masked.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
        return -999;
    }
    return r.exitCode;
}

// The LSan leg — `runAotChecked` flips `ran` to false on a leak, so the -999
// sentinel fails the row on the sanitizer axis independently of the counter.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
        return -999;
    }
    return r.exitCode;
}

// A class whose destructor counts disposals.
constexpr const char* kRes =
    "module M; "
    "class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; ";

// 1. ISSUE REPRO — `using var top := l.Pop();` inside `Holder<T>.Cycle`, where
//    `l : OwningStack<T>` and `Pop()` is declared `owned T` (stdlib §4.11).
//    `top` is the sole disposal point for the popped object. Pre-fix: 0.
//    Post-fix: 100.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           owningstack_pop_typeparam_using_var_disposes) {
    std::string src = std::string(kRes) +
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T): int "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    using var top := l.Pop(); "
        "    return l.Count; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := Holder<Res>.Cycle(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

// 2. The issue's TWO-ELEMENT variant, which separates this defect from #4596
//    by measurement: `Push(a); Push(b); using var top := l.Pop();` leaves ONE
//    element in the collection at scope end and hands the other to `top`. On
//    the post-#4596 / pre-#4616 compiler the issue measured exactly HALF the
//    objects disposed — the remaining element by the collection's scope-end
//    cascade (#4596's half), the popped one by nothing (this issue). 100
//    iterations × 2 objects: pre-fix 100, post-fix 200.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           two_element_pop_disposes_both_halves) {
    std::string src = std::string(kRes) +
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T, owned b: T): int "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    l.Push(move b); "
        "    using var top := l.Pop(); "
        "    return l.Count; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := Holder<Res>.Cycle(new Res(), new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 3. The FIFO sibling: `OwningQueue<T>.Dequeue(): owned T` (stdlib §4.11) is
//    the other name the (b2) arm gates on an owning receiver, and reads the
//    same unresolved `T`. Pre-fix: 0. Post-fix: 100.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           owningqueue_dequeue_typeparam_using_var_disposes) {
    std::string src = std::string(kRes) +
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T): int "
        "    using var q := new OwningQueue<T>(); "
        "    q.Enqueue(move a); "
        "    using var front := q.Dequeue(); "
        "    return q.Count; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := Holder<Res>.Cycle(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

// 4. A generic METHOD's own type parameter on a NON-generic class — the other
//    way a substitution becomes active (§14.1 admits type parameters on both
//    declarations). Pre-fix: 0. Post-fix: 100.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           generic_method_typeparam_pop_disposes) {
    std::string src = std::string(kRes) +
        "class U "
        "  public static method Cycle<T>(owned a: T): int where T: class "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    using var top := l.Pop(); "
        "    return l.Count; "
        "  end Cycle; "
        "end U; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := U.Cycle<Res>(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

// 5. An INTERFACE substitution. (b2)'s disposer guard admits an interface-typed
//    result unconditionally (`isIface`), so resolving the parameter must reach
//    that guard for `T = IThing` too; the disposal is the same vtable slot-2
//    dispatch. Pre-fix: 0. Post-fix: 100.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           interface_substitution_pop_disposes) {
    constexpr const char* kSrc =
        "module M; "
        "public interface IThing "
        "  method Val(): int; "
        "end IThing; "
        "class Res: IThing "
        "  public static _n: int := 0; "
        "  public method Val(): int => 5; "
        "  destructor _n := _n + 1; end destructor; "
        "end Res; "
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T): int "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    using var top := l.Pop(); "
        "    return l.Count; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := Holder<IThing>.Cycle(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kSrc), 100);
}

// 6. CONTROL at the shape that already worked — the SAME body with `Holder`
//    non-generic and the element type spelled concretely. `exprType` is already
//    `Res` there, `resolveTypeParamRef` returns it unchanged, and (b2) always
//    claimed it. This is the differential the issue reports (0 vs 100) and it
//    must read 100 on BOTH compilers.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           nongeneric_concrete_pop_control) {
    std::string src = std::string(kRes) +
        "class Holder "
        "  public static method Cycle(owned a: Res): int "
        "    using var l := new OwningStack<Res>(); "
        "    l.Push(move a); "
        "    using var top := l.Pop(); "
        "    return l.Count; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    _ := Holder.Cycle(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

// 7. NO-OVER-DISPOSE GUARD — the BORROWING family in the same generic body.
//    `Stack<T>.Pop(): T` returns a BORROW (stdlib §4.11: "on the borrowing
//    forms, `Pop`, `Peek`, and `Dequeue` produce borrows"), and the element's
//    real owner is the caller's `using var r`. `isBuiltinOwnedTransferCall`
//    gates `Pop` on an `OwningStack`/`OwningQueue` receiver precisely so this
//    stays a borrow — resolving `T` to `Res` must NOT widen it into an owned
//    slot. If it did, each `Res` would be disposed twice and this would read
//    200 (plus an ASan double-free). 100 on BOTH compilers.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           borrowing_stack_pop_typeparam_does_not_dispose_control) {
    std::string src = std::string(kRes) +
        "class Holder<T> where T: class "
        "  public static method Keep(x: T): int "
        "    using var s := new Stack<T>(); "
        "    s.Push(x); "
        "    using var top := s.Pop(); "
        "    return s.Count; "
        "  end Keep; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<100 do "
        "    using var r := new Res(); "
        "    _ := Holder<Res>.Keep(r); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

// 8. The observable VALUE is unchanged. `l.Count` after the single `Pop` is 0,
//    so the issue's program returns 0 both before and after — this row pins
//    that the fix added a disposal without moving it ahead of the `Count` read
//    (a disposal emitted too early would change the answer or crash).
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           pop_count_value_unchanged_control) {
    constexpr const char* kSrc =
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T, owned b: T): int "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    l.Push(move b); "
        "    using var top := l.Pop(); "
        "    return l.Count + 7; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  return Holder<Res>.Cycle(new Res(), new Res()); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kSrc), 8);
}

// 9. LOOP-AMPLIFIED LEAK ORACLE — the issue's repro at 500 iterations under
//    `runAotChecked` (ASan/LSan on the generated code when the gate forces the
//    sanitizer on). The issue reports "a direct leak of the popped objects"
//    under `--sanitize=address` for the generic form with the concrete twin
//    clean; this is that measurement, looped so the allocation is genuinely
//    unreachable rather than pinned by a live stack slot. Returns
//    `(500 * 3) mod 251` = 245 on both compilers, and is LSan-clean only with
//    the fix.
BEZEL_TEST(generic_body_using_var_typeparam_local_dispose_4616,
           loop_amplified_500x_pop_leak_free) {
    constexpr const char* kSrc =
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Holder<T> where T: class "
        "  public static method Cycle(owned a: T): int "
        "    using var l := new OwningStack<T>(); "
        "    l.Push(move a); "
        "    using var top := l.Pop(); "
        "    return l.Count + 3; "
        "  end Cycle; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 0..<500 do "
        "    acc := acc + Holder<Res>.Cycle(new Res()); "
        "  end; "
        "  return acc mod 251; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(kSrc), 245);
}

}  // namespace
