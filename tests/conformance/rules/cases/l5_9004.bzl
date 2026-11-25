//! case: l5_9004
//! doc: stdlib
//! section: 5
//! title: an eager Sum over a conditional of a method group and a lambda answers per the selected arm
//! rule: are call-bounded as their unmarked signatures state — they invoke the callback during the call and
//!   retain nothing.
//! expect: run
//! output: 2200
//! notes: Harvested from the #9012 review (p6_v2). Both arms compute the identity, so the selected arm cannot
//!   change the answer. Trace per iteration: Sum over [5,6] = 11; 11 x 200 = 2200.
module Conf;
public class Program
  static method P(i: int): int => i;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var a := new Array<int>();
      a.Add(5); a.Add(6);
      t := t + a.Sum((if i mod 2 = 0 then P else lambda(x: int) => x));
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
