// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_relop_string_scrutinee_rejected_1202).
//
// `case x > 5` over a `string` scrutinee — an integer operand over a string
// scrutinee; reject (the symmetric direction). #1202.

module M; class C public static method M(x: string): int return case x of > 5 => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: case pattern operand type is not compatible with the scrutinee type
// CHECK-SAME: (§19)
