// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_override_test.cpp
//   (BEZEL_TEST sema_hide_1694, public_base_member_still_blocks_hiding).
//
// Control: `public` base members still always block (regression guard —
// duplicates hiding_base_field_diagnoses above, restated under the #1694
// naming so the whole blocked-name-set axis is grouped together).

module M; class Base public _x: int; end Base; class Derived : Base public _x: int; end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
