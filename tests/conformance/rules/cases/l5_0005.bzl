//! case: l5_0005
//! doc: stdlib
//! section: 5
//! title: materializing with ToArray() produces a result that may be enumerated more than once
//! rule: To enumerate the results more than once, materialize first --
//!   using var ys := xs.Where(...).ToArray(); produces an owned Array<T>,
//!   which, like any collection, may be enumerated any number of times.
//! cites: 5, 29.11
//! expect: run
//! output: 2
//!   3
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var ys := xs.Where(lambda(x: int): bool => x > 1).ToArray();
    for var v in ys do
    end;
    for var v2 in ys do
      Console.WriteLine(v2);
    end;
  end Main;
end Program;
