// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_relop_type_mismatch_rejected_1202).
//
// `case x > "hi"` over an `int` scrutinee — a string operand cannot be
// compared to an integer; reject (was a compiler abort). #1202.

module M; class C public static method M(x: int): int return case x of > "hi" => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: case pattern operand type is not compatible with the scrutinee type
// CHECK-SAME: (§19)
