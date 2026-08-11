// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §22.16 (v0.240), issue #2942: `result` and `old(E)` are contextual
// keywords valid only inside `ensures` clauses. They are not implemented in
// this revision (#2942); the fix ships a SAFE LOUD REJECT that distinguishes
// the inside-`ensures` case (not-yet-implemented) from the outside-`ensures`
// case (contextual-keyword). This lit pin covers the INSIDE-`ensures` arm.
//
// FAILS-without / PASSES-with: without the fix, `result` / `old` in an
// `ensures` clause fall through ordinary name lookup and bezelc reports
// the generic "unresolved identifier `result`" / "unresolved identifier
// `old`" — no §22.16 citation, no mention of issue #2942. With the fix,
// both keywords surface the §22.16-cited "not yet implemented in `ensures`
// clauses" diagnostic, so FileCheck matches the CHECK lines below.
//
// Axis grid (this pin covers the inside-`ensures` arm):
//   (1) `result` in an `ensures` clause      → not-yet-implemented diagnostic
//   (2) `old(E)` in an `ensures` clause      → not-yet-implemented diagnostic
//
// Other arms (outside-`ensures`, in body/requires/invariant) are covered by
// `contextual_result_old_outside_ensures_2942.bzl` in this same revision.

module M;
class C
  public static method Dbl(n: int): int
    ensures(result = n * 2)
// CHECK: contextual_result_old_in_ensures_2942.bzl:26:13: the contextual keyword `result` is not yet implemented in `ensures` clauses
    return n * 2;
  end Dbl;

  public static method F(n: int): int
    ensures(old(n) = n)
// CHECK: contextual_result_old_in_ensures_2942.bzl:32:13: the contextual keyword `old` is not yet implemented in `ensures` clauses
    return n + 1;
  end F;
end C;
