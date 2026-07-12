// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_field_assignment_747_test.cpp
//   (BEZEL_TEST readonly_field_assignment_747, instance_receiver_rejected).
//
// §22.8: instance-qualified readonly write in a normal method is rejected.

module M; class C public readonly _v: int := 10; public static method Main(): int using var c := new C(); c._v := 50; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_v` outside the declaring class's constructor
// CHECK-SAME: (§22.8)
