//! case: s29_11_4502
//! doc: spec
//! section: 29.11
//! title: a lazy query materialized with ToArray may be enumerated any number of times
//! rule: `using var ys := xs.Where(...).ToArray();` produces an owned `Array<T>`,
//!   which — like any collection — may be enumerated any number of times.
//! cites: 28.3.10
//! expect: run
//! output: 12
//!   30
//!   12
//!   30
//! issue: 8392
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [5, 12, 30];
    using var kept := xs.Where(lambda(x) => x > 10).ToArray();
    for var x in kept do
      Console.WriteLine(x);
    end;
    for var x in kept do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
