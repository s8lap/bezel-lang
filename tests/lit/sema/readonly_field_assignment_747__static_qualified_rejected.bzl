// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/readonly_field_assignment_747_test.cpp
//   (BEZEL_TEST readonly_field_assignment_747, static_qualified_rejected).
//
// §22.8: type-qualified static readonly write in a normal method is rejected.

module M; class C public static readonly Max: int := 10; public static method Main(): int C.Max := 50; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `Max` outside the declaring class's constructor
// CHECK-SAME: (§22.8)
