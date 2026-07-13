// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_user_class_invariant_different_arg_rejected_1180).
//
// §12.9 L1590: "Classes… are always invariant." Box<Dog> to Box<Animal>
// (Box is a user-declared generic class) must be rejected.

module M; class Animal end Animal; class Dog: Animal end Dog; class Box<T> end Box; class C public method M(b: Box<Dog>) var x: Box<Animal> := b; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
