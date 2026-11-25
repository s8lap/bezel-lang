//! case: s20_9413
//! doc: spec
//! section: 20
//! title: a conditional of a bound instance method reference and a static method group selects by condition
//! rule: Conditions are evaluated in textual order; the first `true` condition selects its branch,
//!   the `else` branch is selected when none holds, and only the selected branch is evaluated.
//! cites: 20, 27.6
//! expect: run
//! output: 2400
module Conf;

public class Sel
  public property K: int;
  public constructor(k: int)
    this.K := k;
  end constructor;
  method P(i: int): int => i + this.K;
  method Q(i: int): int => i * 3;
  public method RunThis(c: bool): int
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    return a.Sum((if c then this.P else this.Q));
  end RunThis;
  public method RunMix(c: bool): int
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    return a.Sum((if c then this.P else Sel.S));
  end RunMix;
  public static method S(i: int): int => i - 1;
end Sel;

public class Program
  public static method Main(): int
    var s := new Sel(2);
    var t := 0;
    for var i in 0 ..< 200 do
      t := t + s.RunMix(i mod 2 = 0);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
