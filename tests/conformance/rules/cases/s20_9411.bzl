//! case: s20_9411
//! doc: spec
//! section: 20
//! title: a conditional of two capturing lambdas as an eager Sum selector selects by the condition
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.3
//! expect: run
//! output: 130900
module Conf;

public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var k := i;
      var a := new Array<int>();
      a.Add(5);
      a.Add(6);
      t := t + a.Sum((if i mod 2 = 0 then lambda(x: int) => x + k else lambda(x: int) => x * k));
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
