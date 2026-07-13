// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_literal_out_of_range_byte_rejected_738).

module M; class C public method M() var b: byte := 300; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `b` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
