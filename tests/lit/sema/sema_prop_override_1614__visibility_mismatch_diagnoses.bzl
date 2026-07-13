// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, visibility_mismatch_diagnoses).
//
// §12.1 / #1614: a property override with mismatched visibility is rejected.

module M; class Base public virtual property Name: string => "b"; end Base; class D : Base internal override property Name: string => "d"; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `Name` does not match the base member's visibility
// CHECK-SAME: (§12.1)
