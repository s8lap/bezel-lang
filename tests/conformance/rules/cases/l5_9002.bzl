//! case: l5_9002
//! doc: stdlib
//! section: 5
//! title: a lazy query built before an eager terminal on the same source still enumerates correctly afterwards
//! rule: is retained by the returned query value until enumeration, not just for the duration of the call.
//! expect: run
//! output: 4600
//! notes: Harvested from the #9012 review (p2). The lazy `Where(F)` and `Select(P)` queries are created before
//!   the eager `Sum(P)` terminals on the same source and enumerated after them. Trace per iteration over
//!   [1,2,3]: a.Sum(P) = 6; q = Where(i>1) = [2,3], q.Sum(P) = 5; a.Sum(P) = 6; q2 = Select(P) enumerated
//!   = 6; 23 per iteration x 200 = 4600.
module Conf;
public class Program
  static method F(i: int): bool => i > 1;
  static method P(i: int): int => i;
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var a := new Array<int>();
      a.Add(1); a.Add(2); a.Add(3);
      var q := a.Where(F);
      t := t + a.Sum(P);
      t := t + q.Sum(P);
      var q2 := a.Select(P);
      t := t + a.Sum(P);
      for var x in q2 do t := t + x; end for;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
