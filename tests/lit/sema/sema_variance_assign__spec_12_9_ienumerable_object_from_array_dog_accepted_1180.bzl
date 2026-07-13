// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_ienumerable_object_from_array_dog_accepted_1180).
//
// Covariant to root: Array<Dog> → IEnumerable<Object> (Dog is a ref type) — OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public method M(a: Array<Dog>) var x: IEnumerable<Object> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
