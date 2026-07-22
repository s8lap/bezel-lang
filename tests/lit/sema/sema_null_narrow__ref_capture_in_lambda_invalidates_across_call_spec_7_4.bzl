// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, ref_capture_in_lambda_invalidates_across_call_spec_7_4).
//
// §7.4 (#1566): a lambda that passes the local as a `ref` argument mutates it
// (the callee may null it), so it is mutably captured — a call in the block
// invalidates the narrowing exactly as a direct assignment inside the lambda.

module M; class C public static method Zap(ref u: string?) u := null; end Zap; public static method Run(): int var s: string? := "hi"; var f := lambda() => do Zap(ref s); end; if s <> null then f(); return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
