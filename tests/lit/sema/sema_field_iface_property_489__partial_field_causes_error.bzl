// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, partial_field_causes_error).
//
// §11.2 / §23 — test 6: class provides a property for one interface member but
// only a field for another → the field-backed member gives an error.

module M; interface ITwo property A: int; property B: int; end ITwo; class Two : ITwo public property A: int := 1; public B: int := 2; destructor end destructor; end Two;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Two` does not implement interface property `B` — a field does not satisfy a property contract; declare `public property B: T`
// CHECK-SAME: (§11.2 / §23)
