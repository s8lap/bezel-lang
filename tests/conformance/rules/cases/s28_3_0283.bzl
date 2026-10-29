//! case: s28_3_0283
//! doc: spec
//! section: 28.3
//! title: two aliases and a nullable view of one owned array are borrows of the owner
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 1809
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<int> := [1,2,3];
      var b := a;
      var c := a;
      var n: Array<int>? := a;
      sum := sum + b.Count + c.Count + (n?.Count ?? 0);
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
