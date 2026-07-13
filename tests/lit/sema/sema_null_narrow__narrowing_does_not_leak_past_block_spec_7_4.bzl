// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, narrowing_does_not_leak_past_block_spec_7_4).
//
// §7.4 / §12.7: narrowing is block-scoped — after the controlled block the
// variable is nullable again, so a trailing bare access is an error.

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int if user <> null then var inside: int := user.Name; return inside; end; return user.Name; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
