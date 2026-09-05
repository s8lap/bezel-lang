// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// narrow #3837 (§10.5 verbatim, v0.293): "Enum methods may not be declared
// `virtual`, `abstract`, or `sealed override`; `override` is admitted only
// on the `Object`-virtual members ... — Equals, GetHashCode, ToString —
// ... and is rejected on every other enum method." `sealed override` is
// named as its OWN rejected compound, distinct from plain `override` — it
// is never admitted here, even on a trio member. A REGRESSION for the
// #3837 admission fix itself: the first cut of that fix admitted
// `override` on the trio purely by name+arity, without checking `sealed`,
// so `sealed override GetHashCode` on an enum was silently ADMITTED
// (loud-to-quiet). This row pins the correct (loud) rejection.

module M; enum Priority Low, Medium, High; public sealed override method GetHashCode(): int => int(this); end Priority; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `GetHashCode` may not be virtual, abstract, or override
// CHECK-SAME: (§10.5)
