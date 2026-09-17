//! case: s29_11_0003
//! doc: spec
//! section: 29.11
//! title: materializing a lazy query first permits enumerating the result any number of times
//! rule: To enumerate the results more than once, materialize first:
//!   `using var ys := xs.Where(...).ToArray();` produces an owned
//!   `Array<T>`, which — like any collection — may be enumerated any number
//!   of times.
//! cites: 29.11
//! expect: run
//! stdout: 10
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var ys := xs.Where(lambda(x) => x > 1).ToArray();
    var total := 0;
    for var v in ys do
      total := total + v;
    end;
    for var w in ys do
      total := total + w;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
