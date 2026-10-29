//! case: s29_2_0018
//! doc: spec
//! section: 29.2
//! title: for-in over a peeled owned-producer call disposes the generator once per loop
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). The source `h.Mk()!` (an owned nullable producer peeled by `!`) is consumed: 100 loops sum 1+2 each (300) and the generator `finally` ran 100 times, once per loop.
//! cites: 29.2, 29.1, 31.2
//! expect: run
//! stdout: sum=300 fins=100
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
      for var x in h.Mk()! do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;
