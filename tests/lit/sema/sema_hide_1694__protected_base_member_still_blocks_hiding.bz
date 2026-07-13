// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, protected_base_member_still_blocks_hiding).
//
// Control: `protected` base members always block — "a subclass reached the
// base by inheriting from it," so a protected member is always visible at
// the derived declaration site (§12.2 v0.184).

module M; class Base protected _x: int; end Base; class Derived : Base protected _x: int; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
