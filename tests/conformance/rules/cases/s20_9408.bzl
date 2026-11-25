//! case: s20_9408
//! doc: spec
//! section: 20
//! title: a conditional of two method groups as a call argument selects the same branch in either arm order
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.6
//! expect: run
//! output: 5600
module Conf;

public class Program
  static method P(i: int): int => i;
  static method Q(i: int): int => i * 3;
  static method Use(f: (int) -> int): int => f(7);
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      t := t + Use((if i mod 2 = 0 then P else Q));
      t := t + Use((if i mod 2 = 0 then Q else P));
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
