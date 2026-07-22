// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, contravariant_param_narrowing_rejected_spec_27_1_1).
//
// §27.1.1 first bullet, reverse direction: a (Dog) -> () value is NOT usable
// where (Animal) -> () is expected — calling the result with a non-Dog
// Animal would call the Dog-only handler on the wrong receiver shape.

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method Main(): int var f: (Dog) -> () := lambda(d: Dog) => do end; var g: (Animal) -> () := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
