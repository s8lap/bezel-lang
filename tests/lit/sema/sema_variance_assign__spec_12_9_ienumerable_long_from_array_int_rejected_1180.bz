// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_ienumerable_long_from_array_int_rejected_1180).
//
// Array<int> → IEnumerable<long>: int is a value type, long ≠ int → rejected.

module M; class C public method M(a: Array<int>) var x: IEnumerable<long> := a; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
