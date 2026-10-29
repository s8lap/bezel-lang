//! case: s28_3_0279
//! doc: spec
//! section: 28.3
//! title: a borrowing alias and a nullable view of a borrowed parameter are read-only views
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1206
module Conf;
class Program
  public static method F(a: Array<int>): int
    var b := a;
    var c: Array<int>? := a;
    return b.Count + (c?.Count ?? 0);
  end F;
  public static method Main(): int
    var sum := 0;
    var xs: Array<int> := [1,2,3];
    for var i in 0..200 do
      sum := sum + Program.F(xs);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
