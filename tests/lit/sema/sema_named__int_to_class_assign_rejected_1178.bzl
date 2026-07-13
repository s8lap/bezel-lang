// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_assignability_test.cpp
//   (BEZEL_TEST sema_named, int_to_class_assign_rejected_1178).

module M; class Dog end Dog; class C public method M() var d: Dog := 5; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `d` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
