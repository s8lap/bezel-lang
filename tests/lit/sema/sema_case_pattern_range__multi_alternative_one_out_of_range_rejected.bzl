// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, multi_alternative_one_out_of_range_rejected).
//
// A comma-separated alternative arm rejects when ANY alternative is out of
// range — `1, 2, 256` over a byte still flags 256 (§10.6 / §5.1). #1153.

module M; class C public static method M(x: byte): int return case x of 1 | 2 | 256 => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `256` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
