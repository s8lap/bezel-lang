//! case: s29_2_0033
//! doc: spec
//! section: 29.2
//! title: The generator finally runs before the statement after a peeled local for-in
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `e!` over a nullable local holding a generator: it, it, fin, end.
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
    var e: IEnumerable<int>? := h.GenS();
    for var x in e! do Console.WriteLine("it"); end;
    Console.WriteLine("end");
    return 0;
  end Main;
end Program;
