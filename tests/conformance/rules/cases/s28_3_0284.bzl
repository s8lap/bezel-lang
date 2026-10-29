//! case: s28_3_0284
//! doc: spec
//! section: 28.3
//! title: an alias declared in one branch and a direct read in the other both see the owner
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 603
module Conf;
class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0..200 do
      var a: Array<int> := [1,2,3];
      if i mod 2 = 0 then
        var b := a;
        sum := sum + b.Count;
      else
        sum := sum + a.Count;
      end;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
