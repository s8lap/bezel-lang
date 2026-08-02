// RUN: %bezelc -c -o %t.o %s
//
// §9.7 / §9.1 (v0.226, issue #1251): the ACCEPT (non-over-rejection) control
// for the span field-store rule. §9.7's storage bullet is scoped to "an
// instance field of a **class**" — its premise is that "a class instance's
// lifetime is not bounded by any borrow source's scope". A RECORD's own
// component is a different thing: §9.7's transitivity clause counts a
// borrow-carrying field as exactly what MAKES a record borrow-bearing, so a
// body record's constructor storing a span into its own field is legal, and
// §9.1's synthesized primary constructor for the identical positional record
// performs the very same store. Rejecting it would make a borrow-bearing body
// record unconstructible.
//
// Three accepted shapes, one per boundary the fix must not cross:
//
//   1. `SpanBoxA` — a body record whose constructor stores a span into its own
//      plain field (the bare implicit-`this` write form).
//   2. `SpanBoxB` — the same via the `this.`-qualified property write form.
//   3. `StrView` — a `ReadOnlyStringSpan` in a CLASS instance field: it views a
//      runtime-managed `string`, which §9.7 excludes from borrow-bearing
//      ("runtime-managed values have unbounded lifetime by construction"), so
//      it is not a span for this rule and stays storable.
//
// Plus `BackRef`, §28.3's DIRECT class-typed borrow field, which §9.7 states is
// "unchanged" by this bullet and stays holder-kind-conditioned.

module M; record SpanBoxA public v: ArraySpan<int>; public constructor(x: ArraySpan<int>) v := x; end constructor; end SpanBoxA; record SpanBoxB public readonly property V: ArraySpan<int>; public constructor(x: ArraySpan<int>) this.V := x; end constructor; end SpanBoxB; class StrView public v: ReadOnlyStringSpan; public constructor(s: ReadOnlyStringSpan) v := s; end constructor; end StrView; class Res public constructor() end constructor; public method Val(): int return 5; end Val; destructor end destructor; end Res; class BackRef public r: Res; public constructor(x: Res) r := x; end constructor; public method Rebind(x: Res) this.r := x; end Rebind; end BackRef; class C public static method Main(): int using var arr := new Array<int>(); arr.Add(1); var a := new SpanBoxA(arr.AsSpan()); var b := new SpanBoxB(arr.AsSpan()); using var sv := new StrView("hi".AsSpan()); using var w := new Res(); using var br := new BackRef(w); br.Rebind(w); return a.v.Length + b.V.Length + sv.v.Length + br.r.Val(); end Main; end C;
