// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_literal_out_of_range_sbyte_rejected_738).
//
// #738 / §5.1: an int constant that does NOT fit the narrower target is rejected
// (no silent truncation). `300` does not fit `sbyte` (-128..127) or `byte`
// (0..255); a negative literal does not fit an unsigned target.

module M; class C public method M() var y: sbyte := 300; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `y` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
