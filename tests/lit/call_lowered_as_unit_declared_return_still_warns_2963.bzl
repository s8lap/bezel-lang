// fix #2963 (codegen; language spec §21.7, §3/#605 permissive typing, v0.240):
// the NEGATIVE control for the sibling accept test (this dir,
// call_lowered_as_unit_no_degrade_note_2963.bzl).
//
// #2963's exemption is GATED on the callable's return type being the unit type,
// because §21.7 states the demand as a language rule: a callable with a return
// type that yields a VALUE wants one, so an arrow body that produced none there
// IS a real degrade and must keep saying so. Widening the exemption to "any
// arrow body" would silence that, which is the exact shape of mistake #2966
// records the counterexample for — so it is pinned here rather than left to
// review.
//
// fix #3434 (§4.7 / §21.7, v0.283) narrowed how that gate is SPELLED without
// changing what this file pins. It used to be `arrowBodyLoweredAsUnit`'s
// `!declRet` — a test on whether a return type was WRITTEN — which silenced
// nothing here (`: int` is written) but wrongly warned on `: ()`, a written
// return type that §4.7 makes the SAME type as writing none. The gate now asks
// sema's own authority (`sema::ascribedReturnTypeProducesValue`, which
// `ExprTC::returnsValue` also calls) whether the resolved return type produces a
// value. `: int` does, so this file's CHECK is unchanged — and its sharper
// sibling, arrow_body_nullable_unit_return_still_warns_3434.bzl, pins the
// `()?` case the new gate must also keep warning on.
//
// The degrading body is an INDEXER read `x[0]` on a bare type parameter `T`:
// §3.6 rule 2 / §14.2 leave a bare type parameter's member set open, so sema
// ADMITS the read, while the monomorphized clone at `T = int` has no indexer
// to dispatch to — so the arrow body reaches the #2491 site with no value and
// no IR emitted — "declined", not "lowered as unit".  `Get` is CALLED, because
// an uninstantiated class's body is never emitted and the note would then be
// absent for a reason that has nothing to do with this fix.
//
// ⚠ THE TRIGGER WAS RE-BASED BY fix #7973 (§8.1, v0.350). The degrading body
// used to be `o.Anything()` on an `Object` receiver — the §3/#605 permissive
// surface. §8.1 declares the root `Object`'s public surface ("The declaration
// above defines the public surface"), and §26.5 resolves a member against the
// receiver's declared type, so that shape is now a COMPILE ERROR and never
// reaches codegen. The subject of this case — #2963's gate — is unchanged.
//
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class C
  public method Get<T>(x: T): int => x[0];
  public static method Main(): int
    using var c := new C();
    return c.Get(1);
  end Main;
end C;

// CHECK: internal: return-value expression failed to lower to a value
// CHECK-SAME: #2491
