// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, task_generic_bare_statement_is_error_1497).
//
// REJECT: bare `Work();` statement, `Work(): Task<int>` — #1497.

module M; class C public static async method Work(): Task<int> return 0; end Work; public static async method Caller(): Task Work(); return; end Caller; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression statement discards a `Task` result — an unawaited task's exceptions and lifetime are silently dropped; `await` it, use `Task.FireAndForget(expression);` for supervised fire-and-forget, or bind it explicitly (`using var t := expression;`) to dispose it at scope end
// CHECK-SAME: (§21.9)
