// Regression for #1657 (§28.3 / §29.8 / §30.1, v0.184).
//
// An `owned` parameter of an ITERATOR (`sawYield_`) or ASYNC (`isAsync_`)
// method that the body never discharges is disposed by the state-machine
// frame at teardown (§29.8 for the iterator, §30.1 for the async method).
// Per §28.3 (v0.184) "The rule is uniform across body kinds": that implicit
// frame disposal carries the must-use warning, attached at the parameter
// declaration — exactly as the SYNCHRONOUS owned-parameter auto-drop does.
//
// FAIL-WITHOUT / PASS-WITH: before the fix the `sawYield_ || isAsync_` branch
// in Ownership.cpp set `iteratorAutoDispose` but emitted NO diagnostic, so the
// two warnings below did not fire and this test's CHECK-DAG lines failed to
// match (the sync case, unchanged, has always warned). With the fix the branch
// reports the must-use warning, so both fire. The program is legal (frame
// disposal is sound), so `bezelc -c` must still exit zero.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class Cell public static n: int := 0; public V: int := 7;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor; end Cell;
class P
  public method Wait(): Task => Task.Delay(0);
  // iterator method: owned `ic` never discharged -> §29.8 frame disposal + warn.
  public static method UseIter(owned ic: Cell): IEnumerable<int>
    yield return 1;
  end UseIter;
  // async method: owned `ac` never discharged -> §30.1 frame disposal + warn.
  public async method UseAsync(owned ac: Cell): Task
    await Wait();
  end UseAsync;
end P;

// CHECK-DAG: owned parameter `ic` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop)
// CHECK-DAG: owned parameter `ac` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop)
