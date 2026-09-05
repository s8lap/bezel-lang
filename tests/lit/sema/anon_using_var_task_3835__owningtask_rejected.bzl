// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3835 / spec §17.13 "The anonymous owning binding `using var _`"
// (v0.288, axiom A6): same rejection over `OwningTask<T>` (Pattern B) — it
// is still a Task-family type, still un-awaitable once anonymously bound.
//
// REJECT: `using var _ := <OwningTask<T>>`.

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static async method Make(): OwningTask<R> return new R(); end Make; public static method Main(): int using var _ := Make(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot bind a `Task` to the anonymous `using var _` — nothing can await an unnamed binding, so this is the discard's abandonment deferred to scope end; use `Task.FireAndForget(expression)` for supervised fire-and-forget, or a named `using var t := expression;` for the unsupervised scope-end drop
// CHECK-SAME: (§17.13 / §30.10)
