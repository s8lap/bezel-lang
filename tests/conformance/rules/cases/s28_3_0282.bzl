//! case: s28_3_0282
//! doc: spec
//! section: 28.3
//! title: aliases of an owned local minted by an owned-return call are borrows of it
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method Mk(): owned Array<int> => [1,2,3];
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a := Program.Mk();
      var b := a;
      var n: Array<int>? := a;
      sum := sum + b.Count + (n?.Count ?? 0);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
