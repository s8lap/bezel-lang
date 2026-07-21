// Regression for #1657 (§28.3 / §29.8 / §30.1, v0.184) — the warning-free form.
//
// The must-use warning on a frame-disposed `owned` parameter of an iterator or
// async method (see frame_owned_param_autodrop_1657__iterator_async_warn.bzl)
// is SUPPRESSED when the body discharges the parameter — here by adopting it
// into a `using var` (the warning-free spelling per §28.3/§29.8/§30.1). The
// discharge sets `everDischarged_`, so the `sawYield_ || isAsync_` auto-dispose
// branch is skipped entirely and no diagnostic is emitted.
//
// Guards that the fix does NOT over-warn on the idiomatic adoption form.
//
// ACCEPT / NO-DIAGNOSTIC control: `bezelc -c` must exit zero and emit nothing.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class Cell public static n: int := 0; public V: int := 7;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor; end Cell;
class P
  public method Wait(): Task => Task.Delay(0);
  // owned `ic` adopted into a `using var` -> discharged, no warning.
  public static method UseIter(owned ic: Cell): IEnumerable<int>
    using var _ := owned ic;
    yield return 1;
  end UseIter;
  // owned `ac` adopted into a `using var` -> discharged, no warning.
  public async method UseAsync(owned ac: Cell): Task
    using var _ := owned ac;
    await Wait();
  end UseAsync;
end P;

// NODIAG-NOT: error
// NODIAG-NOT: implicitly disposed
