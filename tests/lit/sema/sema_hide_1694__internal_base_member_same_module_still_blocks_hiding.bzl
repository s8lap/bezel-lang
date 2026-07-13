// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, internal_base_member_same_module_still_blocks_hiding).
//
// Control: `internal` base members block when visible at the derived
// declaration site — same module (§13.4/§12.2 v0.184). Cross-module
// `internal` non-blocking is verified via bezelc directly (issue #1694
// verification notes); the sema-only harness here has no second module.

module M; class Base internal _x: int; end Base; class Derived : Base internal _x: int; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
