// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.5 "Result lifetime and task disposal" (Pattern B) / Appendix C.6 (v0.184)
// — issue #1869 (SB01-C2, residual of #1786).
//
// An `OwningTask<T>` for class T is single-consumer (Pattern B). Bound to a
// PLAIN owned local (`var t`, movable), the first `await t` CONSUMES it —
// §30.5 (line ~7399) / Appendix C.6 say the operand is **moved-from per §28.3**
// after the await ("a second await is rejected at compile time as a moved-from
// read"). A straight-line second await is the #1785 case; THIS test pins the
// BRANCH-JOIN slice: `t` is awaited on ONLY ONE arm of an `if`, then awaited
// AGAIN after the join. On the arm that took the first await, the second await
// re-awaits an already-spent task (runtime InvalidOperationException); the join
// therefore leaves `t` MAYBE-consumed and the second await is a compile error.
//
// FAIL-WITHOUT / PASS-WITH: before #1869 the await-consume was spelled a plain
// `Disposed` state, so the `Owned ⊓ Disposed` join resolved to plain Owned (the
// idempotent §28.2 one-branch-Dispose rule, #601) and the maybe-consumed marker
// was never set — the second await compiled clean and raised IOE only at
// runtime. Marking the consume an await-SPEND (upgraded to maybe-consumed at a
// branch join) makes it the static error this CHECK pins; the pre-fix compiler
// emits nothing and this test fails against it.

module M;

class Cell public constructor() end constructor; end Cell;

class C
  public static async method MakeAsync(): OwningTask<Cell>
    return new Cell();
  end MakeAsync;

  public static async method RunTest(flag: bool): Task
    var t := C.MakeAsync();          // plain owned local OwningTask<Cell>
    if flag then
      using var g1 := await t;       // first await consumes t on this arm only
    end;
    using var g2 := await t;         // second await — t maybe-consumed at join
  end RunTest;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot await `t`: it may have already been consumed on another branch
// CHECK-SAME: (maybe-consumed join, §28.3)
