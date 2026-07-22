// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, int_relop_le_operand_over_range_rejected_spec_5_1).
//
// The other relational operators are gated identically — `<= 5000000000` over
// an `int` operand overflows i32 and is rejected (§5.1). #1156.

module M; class C public static method M(x: int): int return (case x of <= 5000000000 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: integer literal does not fit in `int`; add a suffix (`L` for long, `U`/`UL` for unsigned) to widen it
// CHECK-SAME: (§5)
