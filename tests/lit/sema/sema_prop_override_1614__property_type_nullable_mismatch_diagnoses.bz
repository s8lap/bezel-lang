// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, property_type_nullable_mismatch_diagnoses).
//
// §12.1 / #1614: a property override with the base's type under a different
// (nullable) form is NOT a match — the property type is a return type, so the
// §12.1 T→T? parameter widening does NOT apply (no covariance).

module M; class Base public virtual property Name: string => "base"; end Base; class D : Base public override property Name: string? => null; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `Name` does not match the base member's property type
// CHECK-SAME: (§12.1)
