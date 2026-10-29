//! case: s29_2_0028
//! doc: spec
//! section: 29.2
//! title: an async method looping over a peeled owned field across awaits disposes the generator
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `for var x in this.F!` with an `await` in the body: 100 rounds sum 1+2+3 = 600 and the generator `finally` ran 100 times.
//! cites: 29.2, 29.1, 31.2
//! expect: run
//! stdout: sum=600 fins=100
module Conf;
public class H
  public owned F: IEnumerable<int>? := null;
  public static fins: int := 0;
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2; yield return 3;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public async method Run(): Task<int>
    var t := 0;
    for var x in this.F! do t := t + x; await Task.Delay(0); end;
    return t;
  end Run;
end H;
public class Program
  public static async method Main(): Task<int>
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H();
      h.F := h.GenS();
      sum := sum + await h.Run();
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;
