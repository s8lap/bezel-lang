// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, private_method_call_from_outside_diagnoses).

module M; class A private method Helper(): int => 1; end A; class B public method M(a: A) _ := a.Helper; end M; end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Helper` is private to type `A` and not accessible from `B`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
