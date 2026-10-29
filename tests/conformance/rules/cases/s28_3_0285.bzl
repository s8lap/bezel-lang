//! case: s28_3_0285
//! doc: spec
//! section: 28.3
//! title: an alias and its owner passed as plain arguments to the same call are borrows
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method Cnt(xs: Array<int>): int => xs.Count;
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<int> := [1,2,3];
      var b := a;
      sum := sum + Program.Cnt(b) + Program.Cnt(a);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
