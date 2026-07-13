// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, real_field_hiding_still_diagnosed).
//
// §12.2 / #680 regression: a genuine field declared in derived with the same
// name as a base field must STILL be rejected (hiding a real member).

module M; class Base public _x: int; end Base; class Derived : Base public _x: int; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
