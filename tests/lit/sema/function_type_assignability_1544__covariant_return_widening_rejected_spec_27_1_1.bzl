// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, covariant_return_widening_rejected_spec_27_1_1).
//
// §27.1.1 second bullet, reverse direction: a () -> Animal value is NOT
// usable as () -> Dog — the caller would receive a non-Dog Animal typed
// as Dog.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method MakeAnimal(): Animal return new Animal(); end MakeAnimal; public static method Main(): int var f: () -> Animal := MakeAnimal; var g: () -> Dog := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
