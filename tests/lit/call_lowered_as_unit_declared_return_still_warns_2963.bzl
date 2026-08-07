// fix #2963 (codegen; language spec §21.7, §3/#605 permissive typing, v0.240):
// the NEGATIVE control for the sibling accept test (this dir,
// call_lowered_as_unit_no_degrade_note_2963.bzl).
//
// #2963's exemption is GATED on the callable declaring NO return type —
// `arrowBodyLoweredAsUnit` tests `!declRet` — because §21.7 states the demand as
// a language rule: a callable with a declared return type wants a value, so an
// arrow body that produced none there IS a real degrade and must keep saying so.
// Widening the exemption to "any arrow body" would silence that, which is the
// exact shape of mistake #2966 records the counterexample for — so it is pinned
// here rather than left to review.
//
// The degrading body is §3/#605's permissive surface: `o.Anything()` on an
// `Object` receiver is accepted by sema BY DESIGN and lowers to nothing at all,
// so the arrow body reaches the #2491 site with no value and no IR emitted —
// "declined", not "lowered as unit".  `Get` is CALLED, because an uninstantiated
// class's body is never emitted and the note would then be absent for a reason
// that has nothing to do with this fix.
//
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public method Get(o: Object): int => o.Anything();
  public static method Main(): int
    using var c := new C();
    using var o := new C();
    return c.Get(o);
  end Main;
end C;

// CHECK: internal: return-value expression failed to lower to a value
// CHECK-SAME: #2491
