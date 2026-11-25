//! case: s20_9409
//! doc: spec
//! section: 20
//! title: a conditional of bound instance method references as a Sum selector selects by the condition
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.6, stdlib 5
//! expect: run
//! output: 4400
module Conf;

public class Sel
  method P(i: int): int => i;
  method Q(i: int): int => i * 3;
end Sel;

public class Program
  public static method Main(): int
    var s := new Sel();
    var t := 0;
    for var i in 0 ..< 200 do
      var a := new Array<int>();
      a.Add(5);
      a.Add(6);
      t := t + a.Sum((if i mod 2 = 0 then s.P else s.Q));
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
