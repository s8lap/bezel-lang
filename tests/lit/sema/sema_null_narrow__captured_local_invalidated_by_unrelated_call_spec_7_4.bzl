// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, captured_local_invalidated_by_unrelated_call_spec_7_4).
//
// §7.4 (#1566): the invalidation is not limited to invoking the capturing closure
// itself — ANY control-transfer operation counts, because the closure could be
// reached indirectly. A call to an unrelated method between the test and the use
// invalidates a mutably-captured local just the same.

module M; class C public static method Touch() end Touch; public static method Run(): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then Touch(); return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
