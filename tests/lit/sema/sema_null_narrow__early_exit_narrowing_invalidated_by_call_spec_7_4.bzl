// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, early_exit_narrowing_invalidated_by_call_spec_7_4).
//
// §7.4 (#1566): the invalidation also applies to a narrowing established by an
// early-exit guard at method-body scope, not only inside an `if` block. Here the
// surviving path is narrowed by `if s = null then return; end;`, then a call
// invalidates the mutably-captured local before the use.

module M; class C public static method Touch() end Touch; public static method Run(): int var s: string? := "hi"; var f := () => do s := null; end; if s = null then return -1; end; Touch(); return s.Length; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
