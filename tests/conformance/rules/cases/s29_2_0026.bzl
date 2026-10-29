//! case: s29_2_0026
//! doc: spec
//! section: 29.2
//! title: for-in over a peeled owning enumerable adopts and disposes each element
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `h.F!` has static type `IOwningEnumerable<R>`, so "the desugaring uses the owning-enumerator path" and each `x` is a plain owned local adopted by `using var u := move x` (§29.2); after 100 loops sum=300 and no `R` is live.
//! cites: 29.2, 28.3
//! expect: run
//! stdout: sum=300 live=0
module Conf;
public class R
  public static live: int := 0;
  public V: int;
  public constructor(v: int) V := v; R.live := R.live + 1; end constructor;
  destructor R.live := R.live - 1; end destructor;
end R;
public class H
  public owned F: IOwningEnumerable<R>? := null;
  public constructor() end constructor;
  public method GenO(): owned IOwningEnumerable<R>
    yield return new R(1); yield return new R(2);
  end GenO;
end H;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H();
      h.F := h.GenO();
      for var x in h.F! do using var u := move x; sum := sum + u.V; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " live=" + R.live.ToString());
    return 0;
  end Main;
end Program;
