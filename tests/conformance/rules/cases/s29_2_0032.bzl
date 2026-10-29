//! case: s29_2_0032
//! doc: spec
//! section: 29.2
//! title: The generator finally runs before the statement after a peeled-field for-in
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `h.F!` over a field holding a generator with a printing `finally`: two iterations, then `fin` (the loop end disposes the enumerator), then `end`.
//! cites: 29.2, 29.1, 31.2
//! expect: run
//! output: it
//!   it
//!   fin
//!   end
module Conf;
public class H
  public owned F: IEnumerable<int>? := null;
  public constructor() end constructor;
  public method GenS(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end;
  end GenS;

end H;
public class Program
  public static method Main(): int
    using var h := new H();
    h.F := h.GenS();
    for var x in h.F! do Console.WriteLine("it"); end;
    Console.WriteLine("end");
    return 0;
  end Main;
end Program;
