// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3835 / spec §17.13 "The anonymous owning binding `using var _`"
// (v0.288, axiom A6): the anonymous `using var _ := expression;` binding is
// rejected when the initializer is `Task`-shaped — nothing can await an
// unnamed binding, so it is the discard statement's abandonment deferred to
// scope end.
//
// REJECT: `using var _ := <Task>` over a void async call.

module M; class C public static async method Work(): Task return; end Work; public static method Main(): int using var _ := Work(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot bind a `Task` to the anonymous `using var _` — nothing can await an unnamed binding, so this is the discard's abandonment deferred to scope end; use `Task.FireAndForget(expression)` for supervised fire-and-forget, or a named `using var t := expression;` for the unsupervised scope-end drop
// CHECK-SAME: (§17.13 / §30.10)
