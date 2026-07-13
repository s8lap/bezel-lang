// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_user_class_invariant_same_arg_accepted_1180).
//
// Box<Dog> to Box<Dog>: same type arg for invariant class — must ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class Box<T> end Box; class C public method M(b: Box<Dog>) var x: Box<Dog> := b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
