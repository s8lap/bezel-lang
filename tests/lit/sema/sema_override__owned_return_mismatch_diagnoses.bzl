// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, owned_return_mismatch_diagnoses).

module M; class T end T; class Base public virtual method F(): owned T => new T(); end Base; class D : Base public override method F(): T => new T(); end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `F` does not match the base member's `owned` return modifier
// CHECK-SAME: (§12.1)
