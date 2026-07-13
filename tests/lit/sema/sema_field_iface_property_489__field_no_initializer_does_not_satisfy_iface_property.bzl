// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, field_no_initializer_does_not_satisfy_iface_property).
//
// §11.2 / §23 — test 2: field with no initializer, same scenario.

module M; interface IBar property Count: int; end IBar; class Counter : IBar public Count: int; public constructor() Count := 0; end constructor; destructor end destructor; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Counter` does not implement interface property `Count` — a field does not satisfy a property contract; declare `public property Count: T`
// CHECK-SAME: (§11.2 / §23)
