// Ported from tests/sema/type_param_bound_1620_test.cpp
//   (BEZEL_TEST type_param_bound_1620, type_valid_instantiation_accepted).
//
// §14.2 ANTI-OVER-REJECT: `Box<Dog, Animal>` — `Dog` IS a subclass of
// `Animal`, so `where T: U` (U := Animal) is satisfied — accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class Box<T, U> where T: U where U: Animal public item: T; end Box; class C public static method Main(): int var b: Box<Dog, Animal>? := null; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
