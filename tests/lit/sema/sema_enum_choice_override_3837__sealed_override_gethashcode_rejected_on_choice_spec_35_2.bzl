// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// narrow #3837 (§35.2 verbatim, v0.293): "Choice methods are statically
// dispatched; `virtual` and `abstract` are rejected as on enums (§10.5),
// `override` is admitted exactly where §10.5 admits it" — and §10.5 names
// `sealed override` as its OWN rejected compound, distinct from plain
// `override`, never admitted even on a trio member. The choice-side twin
// of the enum regression above: the first cut of the #3837 fix admitted
// `override` on the trio purely by name+arity, without checking `sealed`,
// so `sealed override GetHashCode` on a choice was silently ADMITTED
// (loud-to-quiet). This row pins the correct (loud) rejection.

module M; choice Shape Circle(radius: double), Square(side: double); public sealed override method GetHashCode(): int => 0; end Shape; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `GetHashCode` may not be virtual, abstract, or override
// CHECK-SAME: (§35.2)
