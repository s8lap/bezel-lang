// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, bare_member_on_nullable_class_rejected_spec_7_4).
//
// §7.4: a direct `user.Name` on a nullable `User?` with NO narrowing is a
// compile error.

module M; class User public Name: int := 0; end User; class C public static method Run(user: User?): int return user.Name; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Name` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
