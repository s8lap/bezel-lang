// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/type_param_bound_1620_test.cpp
//   (BEZEL_TEST type_param_bound_1620, type_unsound_instantiation_rejected).
//
// §14.2: `Box<Cat, Animal>` — `Cat` is not assignable to `Animal`, so the
// `where T: U` (U := Animal) obligation is UNSATISFIED — rejected.

module M; class Animal end Animal; class Dog: Animal end Dog; class Cat end Cat; class Box<T, U> where T: U where U: Animal public item: T; end Box; class C public static method Main(): int var b: Box<Cat, Animal>? := null; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Cat` does not satisfy the constraint `where T: Animal`
// CHECK-SAME: (§14.2)
