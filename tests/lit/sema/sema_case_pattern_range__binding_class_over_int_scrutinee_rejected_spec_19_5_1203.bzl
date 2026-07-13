// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_class_over_int_scrutinee_rejected_spec_19_5_1203).
//
// §19.5 (#1203): `var c: SomeClass` over an `int` scrutinee — a value-type
// primitive is never a class (a heap object). Was previously a SIGABRT in
// codegen (emitIsA called on an i32, not a pointer).

module M; class Dog end Dog; class C public static method M(x: int): int return case x of var d: Dog => 1, _ => 2 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: binding pattern type is incompatible with the scrutinee's static type — a value-type scrutinee can never match a reference-typed binding
// CHECK-SAME: (§19.5)
