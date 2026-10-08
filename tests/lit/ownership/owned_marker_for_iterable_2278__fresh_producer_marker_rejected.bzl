// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// REJECTION test. The `owned` marker on a call argument is a parse error
// per §A.4 / proposal 0090 (v0.246). The program uses `owned C.Make()` at
// line 41; the parse error fires before any sema analysis.
//
// (Before v0.246 the sema "fresh producer" check named why `owned` on a call
// returning `owned T` was an error. That check is now unreachable — parse
// rejects first.)
//
// Fails-without / passes-with: this file pins the rejection so a future reading
// of the diagnostic as "these four forms are admissible" cannot silently
// re-admit the marker here.

module M;
class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
class C
  public static method Make(): owned OwningArray<OwningTask<Cell>>
    return new OwningArray<OwningTask<Cell>>();
  end Make;
  public static async method Main(): Task<int>
    var count := 0;
    await for var tk in Task.WhenEachOwned(owned C.Make()) do
      using var x := await tk;
      count := count + 1;
    end;
    return count;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: invalid use of 'owned' at argument position
// CHECK-SAME: (§A.4, proposal 0090, v0.246)
