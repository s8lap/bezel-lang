//! case: s29_2_0029
//! doc: spec
//! section: 29.2
//! title: for-in over a peeled property that yields an owned array element source sums its elements
//! rule: A `for` loop over a source desugars with `using var __source := source;` and `using var __enumerator := ...GetEnumerator();`, and "both `__source` and `__enumerator` are disposed on every exit path" (§29.2, v0.350); disposing a suspended generator runs its pending `finally` (§29.1, §31.2). `h.Items!` over a nullable property returning the owned array as `IEnumerable<int>?`: sum=600 over 100 rounds (1+2+3 each).
//! cites: 29.2, 7.4
//! expect: run
//! stdout: sum=600
module Conf;
public class H2
  private owned _items: Array<int>?;
  public constructor() _items := [1, 2, 3]; end constructor;
  public property Items: IEnumerable<int>? get => _items; end Items;
end H2;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H2();
      for var x in h.Items! do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString());
    return 0;
  end Main;
end Program;
