// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, narrowing_param_nullability_override_diagnoses).
//
// §12.1 (v0.181, #1560): the reverse direction — base `T?`, override `T` —
// is NOT a match and stays rejected: a narrowing override would let a
// `null` admitted through the base's static type reach a non-nullable
// parameter (mirrors §11.2's identical reverse-direction rejection for
// interface implementations).

module M; class Base public virtual method F(a: string?) _ := 1; end F; end Base; class D : Base public override method F(a: string) _ := 2; end F; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's parameter types
// CHECK-SAME: (§12.1)
