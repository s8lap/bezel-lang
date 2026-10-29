//! case: s28_3_0286
//! doc: spec
//! section: 28.3
//! title: an alias declared in a loop over an owner declared outside the loop is a borrow
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 606
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    var a: Array<int> := [1,2,3];
    for var i in 0..200 do
      var b := a;
      sum := sum + b.Count;
    end;
    sum := sum + a.Count;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
