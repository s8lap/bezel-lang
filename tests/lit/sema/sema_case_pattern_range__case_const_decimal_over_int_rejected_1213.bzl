// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_const_decimal_over_int_rejected_1213).
//
// `case x 1.0m` over an `int` scrutinee — decimal constant over int is also
// an ICmp type mismatch; must be rejected with the same §19 diagnostic. #1213.

module M; class C public static method M(x: int): int return (case x of 1.0m => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: case pattern operand type is not compatible with the scrutinee type
// CHECK-SAME: (§19)
