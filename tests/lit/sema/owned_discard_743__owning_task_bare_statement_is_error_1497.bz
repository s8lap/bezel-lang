// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, owning_task_bare_statement_is_error_1497).
//
// REJECT: bare `Make();` statement, `Make(): OwningTask<R>` (Pattern B) — #1497.

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static async method Make(): OwningTask<R> return new R(); end Make; public static async method Caller(): Task Make(); return; end Caller; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression statement discards a `Task` result — an unawaited task's exceptions and lifetime are silently dropped; `await` it, use `Task.FireAndForget(expression);` for supervised fire-and-forget, or bind it explicitly (`using var t := expression;`) to dispose it at scope end
// CHECK-SAME: (§21.9)
