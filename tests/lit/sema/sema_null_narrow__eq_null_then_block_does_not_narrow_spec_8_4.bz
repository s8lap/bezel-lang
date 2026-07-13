// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, eq_null_then_block_does_not_narrow_spec_8_4).
//
// §7.4: the then-block of `if user = null` does NOT narrow (the value is known
// to be null there) — a member access there is an error.

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int if user = null then return user.Name; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
