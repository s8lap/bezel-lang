// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, property_type_mismatch_diagnoses).
//
// §12.1 / #1614: a property override with a MISMATCHED property type is
// rejected (previously silently accepted — the bug this test pins).

module M; class Base public virtual property Name: string => "base"; end Base; class D : Base public override property Name: int => 42; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `Name` does not match the base member's property type
// CHECK-SAME: (§12.1)
