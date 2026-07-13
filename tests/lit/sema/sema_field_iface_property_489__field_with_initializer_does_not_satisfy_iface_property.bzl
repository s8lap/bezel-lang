// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, field_with_initializer_does_not_satisfy_iface_property).
//
// §11.2 / §23 — test 1: exact repro from bug #489.
// `public Val: int := 0` is a field, not a property; it cannot satisfy
// `interface IFoo property Val: int`. A clean conformance error is required.

module M; interface IFoo property Val: int; end IFoo; class Box : IFoo public Val: int := 0; public constructor(v: int) Val := v; end constructor; destructor end destructor; end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Box` does not implement interface property `Val` — a field does not satisfy a property contract; declare `public property Val: T`
// CHECK-SAME: (§11.2 / §23)
