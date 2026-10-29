//! case: s28_3_0281
//! doc: spec
//! section: 28.3
//! title: aliases of Array elements read through an index are borrows of the element
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 603
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    var xs: Array<Array<int>> := [[1,2],[3]];
    for var i in 0..200 do
      var b := xs[0];
      var n: Array<int>? := xs[1];
      sum := sum + b.Count + (n?.Count ?? 0);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
