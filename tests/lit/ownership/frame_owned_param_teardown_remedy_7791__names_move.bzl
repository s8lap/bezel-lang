// Regression for #7791 (§28.3 *Auto-drop (affine)*, §28.10 *The anonymous
// binding*, §17.13 axiom A6, v0.350).
//
// The must-use warning on a frame-disposed `owned` parameter of an async or
// iterator method names the adoption remedy that discharges it. The remedy used
// to read "adopt it with `using var _ := owned X`": the `owned` spelling is the
// retired binding-position marker (`move` since v0.286), and for a `Task`-family
// parameter the ANONYMOUS binding is itself a compile error (§28.10: "A
// `Task`-family value may not be bound this way (§17.13, axiom A6)"; "`using var
// _ := move x;` is never offered as its remedy"). The remedy now names `move`,
// and the NAMED `using var t2 := move t;` for a Task-family parameter.
//
// Three emission sites carry the remedy (Ownership.cpp, the state-machine
// `owned`-parameter arms): the endState never-discharged arm (Never*), the
// throw-only-exit arm (Throw*), and the conditionally-adopted arm (Cond*, which
// is reached only for a parameter whose adoption the frame-nullable scan
// admits).
//
// FAIL-WITHOUT / PASS-WITH: before the fix every line below read
// "`using var _ := owned X`", so the CHECK-DAG lines did not match and the
// CHECK-NOT guard fired.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=STALE %s

module M;
class Cell public constructor() end constructor; destructor end destructor; end Cell;
class P
  // endState arm, Task-family parameter: the named form.
  public static async method NeverTask(owned t: Task): Task
    Console.WriteLine("n");
  end NeverTask;
  // endState arm, class parameter: the anonymous `move` form.
  public static async method NeverCell(owned c: Cell): Task
    Console.WriteLine("n");
  end NeverCell;
  // throw-only-exit arm, Task-family parameter.
  public static async method ThrowTask(owned t: Task): Task
    throw new InvalidOperationException("x");
  end ThrowTask;
  // throw-only-exit arm, class parameter.
  public static async method ThrowCell(owned c: Cell): Task
    throw new InvalidOperationException("x");
  end ThrowCell;
  // conditionally-adopted arm (the frame-nullable scan admits the `owned`
  // marker spelling), class parameter.
  public static async method CondCell(owned c: Cell, f: bool): Task
    if f then
      using var _ := owned c;
    end;
  end CondCell;
end P;

// CHECK-DAG: warning: owned parameter `t` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var t2 := move t;` (the anonymous `using var _` is rejected for a `Task`, §17.13), to discharge it explicitly
// CHECK-DAG: warning: owned parameter `c` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using var _ := move c;`, to discharge it explicitly
// CHECK-DAG: warning: owned parameter `c` is implicitly disposed by the frame at teardown on the path that does not adopt it (§28.3/§29.8/§30.1 must-use / auto-drop); adopt it on every path with `using var _ := move c;`, to discharge it explicitly
// STALE-NOT: using var _ := owned
