// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_ienumerable_string_from_array_dog_rejected_1180).
//
// Array<Dog> → IEnumerable<string>: Dog is not a subtype of string → rejected.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Dog>) var x: IEnumerable<string> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
