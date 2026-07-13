// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, bare_member_on_nullable_record_rejected_spec_7_4).
//
// §7.4: a direct member read on a nullable RECORD value (`Box?`) with no guard
// is likewise rejected.

module M; record Box(V: int); class C public static method Run(b: Box?): int return b.V; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.V` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
