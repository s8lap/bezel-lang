//! case: s29_2_0030
//! doc: spec
//! section: 29.2
//! title: for-in over a peeled nullable LinkedList local sums its elements
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `l!` over a `LinkedList<int>?` local: 100 rounds of 1+2: sum=300.
//! cites: 29.2, 7.4
//! expect: run
//! stdout: sum=300
module Conf;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      var l: LinkedList<int>? := new LinkedList<int>();
      l!.AddLast(1); l!.AddLast(2);
      for var x in l! do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString());
    return 0;
  end Main;
end Program;
