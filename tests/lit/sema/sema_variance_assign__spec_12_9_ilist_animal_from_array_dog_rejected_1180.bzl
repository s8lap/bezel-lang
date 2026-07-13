// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_ilist_animal_from_array_dog_rejected_1180).
//
// §12.9 L1623: IList<T> is invariant; IList<Dog> is assignable to IList<Dog>
// only. Assigning Array<Dog> to IList<Animal> is rejected.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Dog>) var x: IList<Animal> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
