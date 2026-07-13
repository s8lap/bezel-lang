// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, byte_relop_operand_over_range_rejected_spec_5_1).
//
// A relational pattern's operand is a constant compared against the scrutinee.
// `case x (x: byte) > 300 => …` does not fit a byte; before #1156 sema never
// visited the RelOp operand, so codegen's litAtWidth() truncated 300 to i8 44
// and compiled `x > 44` — a wrong match (matched x in 45..255 instead of
// never). Reject it with the same §5.1 narrowing diagnostic. #1156.

module M; class C public static method M(x: byte): int return case x of > 300 => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `300` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
