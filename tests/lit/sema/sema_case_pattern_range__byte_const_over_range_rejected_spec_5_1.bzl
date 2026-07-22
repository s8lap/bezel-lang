// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, byte_const_over_range_rejected_spec_5_1).
//
// `case x (x: byte) 256 => 1, _ => 0` — 256 does not fit a byte; reject it
// (previously truncated to i8 0 and wrongly matched x == 0). #1153.

module M; class C public static method M(x: byte): int return (case x of 256 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `256` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
