// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, sbyte_const_over_max_rejected_spec_5_1).
//
// A negative constant that underflows the (unsigned) byte range is rejected. A
// signed `sbyte` scrutinee with 200 (> 127) is likewise rejected. #1153.

module M; class C public static method M(x: sbyte): int return (case x of 200 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `200` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
