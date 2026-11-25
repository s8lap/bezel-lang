//! case: s20_9404
//! doc: spec
//! section: 20
//! title: a conditional of two method groups passed to a record constructor selects by the condition
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.6
//! expect: run
//! output: 2000
module Conf;

public record R(f: (int) -> int);

public class Program
  static method P(i: int): int => i;
  static method Q(i: int): int => i * 3;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var r := new R((if i mod 2 = 0 then P else Q));
      t := t + r.f(5);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
