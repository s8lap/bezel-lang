//! case: s28_3_0292
//! doc: spec
//! section: 28.3
//! title: an alias of an owned Array of Arrays reads through to the inner arrays
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<Array<int>> := [[1],[2,3]];
      var b := a;
      var n: Array<Array<int>>? := a;
      sum := sum + b.Count + (n?.Count ?? 0) + b[1].Count;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
