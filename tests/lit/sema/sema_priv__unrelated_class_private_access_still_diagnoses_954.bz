// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, unrelated_class_private_access_still_diagnoses_954).
//
// REJECT: an UNRELATED (non-nested) class still cannot access a private member.

module M; public class A private _x: int := 5; destructor end destructor; end A; public class B public static method Peek(a: A): int => a._x; end B;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `_x` is private to type `A` and not accessible from `B`
// CHECK-SAME: (§13.1 — `private` is visible to the declaring class only)
