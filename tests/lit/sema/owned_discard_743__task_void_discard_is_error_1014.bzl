// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, task_void_discard_is_error_1014).
//
// REJECT: `_ := <Task>` discard of a void async call (plain Task, no result).

module M; class C public static async method Work(): Task return; end Work; public static method Main(): int _ := Work(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot discard a `Task` — disposing it abandons a running operation; use `Task.FireAndForget(expr)` for supervised fire-and-forget, or a named `using var t := expr;` for the unsupervised scope-end drop
// CHECK-SAME: (§17.13 / §30.10)
