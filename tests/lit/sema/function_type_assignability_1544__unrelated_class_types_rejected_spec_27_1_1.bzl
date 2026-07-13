// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, unrelated_class_types_rejected_spec_27_1_1).
//
// §27.1.1 first bullet (no relation at all): a param type that is neither a
// sub- nor a supertype of the target's is rejected — Animal and Widget share
// no hierarchy edge.

module M; class Animal end Animal; class Widget end Widget; class C public static method Feed(a: Animal) return; end Feed; public static method Main(): int var f: (Animal) -> () := Feed; var g: (Widget) -> () := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
