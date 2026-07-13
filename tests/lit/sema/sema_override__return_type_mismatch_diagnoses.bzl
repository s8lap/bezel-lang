// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, return_type_mismatch_diagnoses).

module M; class Base public virtual method F(): int => 1; end Base; class D : Base public override method F(): bool => true; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's return type
// CHECK-SAME: (§12.1)
