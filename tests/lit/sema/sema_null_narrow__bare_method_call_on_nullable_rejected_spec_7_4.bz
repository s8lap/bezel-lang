// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, bare_method_call_on_nullable_rejected_spec_7_4).
//
// §7.4: a method CALL on a nullable value with no narrowing is rejected too
// (member access covers method calls per §7.4).

module M; class User public method Greet(): int => 0; end User; class C public static method Run(user: User?): int return user.Greet(); end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Greet` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
