// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, int_range_bound_over_range_rejected_spec_5_1).
//
// A range BOUND out of the scrutinee's range is rejected: `0..5000000000` over
// an `int` (5000000000 > i32 max). Range-pattern bounds were never visited by
// sema, so they slipped past every check and truncated at codegen. #1153.

module M; class C public static method M(x: int): int return case x of 0..5000000000 => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: integer literal does not fit in `int`; add a suffix (`L` for long, `U`/`UL` for unsigned) to widen it
// CHECK-SAME: (§5)
