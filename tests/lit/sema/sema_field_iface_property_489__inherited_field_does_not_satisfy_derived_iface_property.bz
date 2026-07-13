// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_iface_property_489_test.cpp
//   (BEZEL_TEST sema_field_iface_property_489, inherited_field_does_not_satisfy_derived_iface_property).
//
// §11.2 / §23 — test 7: a field declared in a BASE class that is then used to
// satisfy an interface property in the DERIVED class is also rejected.
// The interface is implemented by the derived class, not the base.

module M; class Base public Val: int := 0; destructor end destructor; end Base; interface IFoo property Val: int; end IFoo; class Derived : Base, IFoo destructor end destructor; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Derived` does not implement interface property `Val` — a field does not satisfy a property contract; declare `public property Val: T`
// CHECK-SAME: (§11.2 / §23)
