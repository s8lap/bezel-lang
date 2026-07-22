// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_relop_float_over_int_rejected_1202).
//
// `case x > 1.5` over an `int` scrutinee — a float operand over an integer
// scrutinee (cross-numeric reconcile is a separate §19.3 ruling); reject
// rather than abort the backend. #1202.

module M; class C public static method M(x: int): int return (case x of > 1.5 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: case pattern operand type is not compatible with the scrutinee type
// CHECK-SAME: (§19)
