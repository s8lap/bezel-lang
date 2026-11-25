//! case: s20_9403
//! doc: spec
//! section: 20
//! title: a conditional of two method groups stored into a function-typed property selects by the condition
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.6
//! expect: run
//! output: 2000
module Conf;

public class Holder
  public property F: (int) -> int;
  public constructor()
    this.F := lambda(x: int) => x;
  end constructor;
  public method Call(i: int): int => this.F(i);
end Holder;

public class Program
  static method P(i: int): int => i;
  static method Q(i: int): int => i * 3;
  public static method Main(): int
    var t := 0;
    var h := new Holder();
    for var i in 0 ..< 200 do
      h.F := (if i mod 2 = 0 then P else Q);
      t := t + h.Call(5);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
