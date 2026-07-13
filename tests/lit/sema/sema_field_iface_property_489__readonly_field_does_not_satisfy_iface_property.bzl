// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, readonly_field_does_not_satisfy_iface_property).
//
// §11.2 / §23 — test 3: readonly field also does NOT satisfy interface property.
// A `readonly` field has no getter function; it is still a FieldDecl.

module M; interface IReadable readonly property Id: int; end IReadable; class Item : IReadable public readonly Id: int; public constructor(i: int) Id := i; end constructor; destructor end destructor; end Item;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Item` does not implement interface property `Id` — a field does not satisfy a property contract; declare `public property Id: T`
// CHECK-SAME: (§11.2 / §23)
