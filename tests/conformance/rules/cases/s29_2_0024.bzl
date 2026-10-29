//! case: s29_2_0024
//! doc: spec
//! section: 29.2
//! title: nested for-in loops over a peeled owned field run each generator finally
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). Outer yields 2 elements, each inner loop re-enumerates `h.F!` (2 elements): 3 enumerators per round, so sum=600 and fins=300.
//! cites: 29.2, 29.1, 31.2
//! expect: run
//! stdout: sum=600 fins=300
module Conf;
public class H
  public owned F: IEnumerable<int>? := null;
  public static fins: int := 0;
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Mk(): owned IEnumerable<int>?
    return this.GenS();
  end Mk;
end H;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H();
      h.F := h.GenS();
      for var x in h.F! do for var y in h.F! do sum := sum + y; end; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;
