// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, unknown_method_on_class_diagnosed).
//
// §11.3 / §12.1: a method name that no member of the class has.

module M; class Animal public method Speak(): int => 1; end Animal; class C public method M(a: Animal): int => a.NoSuchMethod(); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NoSuchMethod` on type `Animal`
// CHECK-SAME: (§11.3, §12.1)
