//! case: s29_2_0027
//! doc: spec
//! section: 29.2
//! title: a breaking for-in inside a generator over a peeled field disposes the inner generator
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). An iterator `Outer` wraps `for var x in this.F!`; the consumer breaks at y=2, so the outer generator is disposed early and the nested enumerator with it: sum=1+2=3 per round (300), inner `finally` once per round (fins=100).
//! cites: 29.2, 29.1, 31.2
//! expect: run
//! stdout: sum=300 fins=100
module Conf;
public class H
  public owned F: IEnumerable<int>? := null;
  public static fins: int := 0;
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2; yield return 3;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Outer(): owned IEnumerable<int>
    for var x in this.F! do yield return x; end;
  end Outer;
end H;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H();
      h.F := h.GenS();
      for var y in h.Outer() do sum := sum + y; if y = 2 then break; end; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;
