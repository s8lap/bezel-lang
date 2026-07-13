// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_field_on_class_diagnosed).
//
// §11.3 / §12.1: a field name that no member of the class has.

module M; class Animal public _legs: int; end Animal; class C public method M(a: Animal): int => a.NoSuchField; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NoSuchField` on type `Animal`
// CHECK-SAME: (§11.3, §12.1)
