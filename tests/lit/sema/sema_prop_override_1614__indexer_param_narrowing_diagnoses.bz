// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, indexer_param_narrowing_diagnoses).
//
// §23 / §12.1 (v0.181, #1560) / #1614: the reverse direction — base `T?`,
// override `T` — is a narrowing and is rejected (a null admitted through the
// base's static type would reach a non-nullable index parameter).

module M; class Base public virtual property [s: string?]: int => 0; end Base; class D : Base public override property [s: string]: int => 0; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `indexer` does not match the base member's index parameter types
// CHECK-SAME: (§12.1)
