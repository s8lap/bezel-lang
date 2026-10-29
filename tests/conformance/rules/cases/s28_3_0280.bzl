//! case: s28_3_0280
//! doc: spec
//! section: 28.3
//! title: aliases of a nullable parameter are borrows, whether the argument is present or null
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method F(p: Array<int>?): int
    var q := p;
    var r: Array<int>? := p;
    return (q?.Count ?? 0) + (r?.Count ?? 0);
  end F;
  public static method Main(): int
    var sum := 0;
    var keep: Array<int> := [1,2,3];
    for var i in 0..200 do
      sum := sum + Program.F(keep);
      sum := sum + Program.F(null);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
