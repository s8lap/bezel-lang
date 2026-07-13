// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, reassignment_invalidates_narrowing_spec_7_4).
//
// §7.4 / §12.7: reassignment within the controlled block invalidates the
// narrowing — a subsequent bare member access becomes an error again.

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?, other: User?): int if user <> null then user := other; return user.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
