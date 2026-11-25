//! case: s28_14_9904
//! doc: spec
//! section: 28.14
//! title: a method-group local passed to an abstract member runs in a 200x loop
//! rule: The caller side is correspondingly unrestricted: any closure — capture-bearing or not — may be
//!   passed to a call-bounded parameter
//! cites: 28.14, 28.9, 27.5
//! expect: run
//! stdout: 19900
//! issue: 9150
//! notes: Harvested from the #9150 review (f_method_group). The method reference `th.Get` captures `th`
//!   and is passed to a call-bounded parameter of an abstract member; each pass adds j, so the total
//!   is 0 + 1 + ... + 199 = 19900.
module Conf;

public abstract class Fl
  public abstract method M(q: () -> int): int;

  public constructor()
  end constructor;
end Fl;

public class Sub: Fl
  public constructor()
  end constructor;

  public override method M(q: () -> int): int => q();
end Sub;

public class Thing
  public K: int;

  public constructor(k: int)
    this.K := k;
  end constructor;

  public method Get(): int => this.K;
end Thing;

public class Program
  public static method Main()
    var t := 0;
    var fl: Fl := new Sub();
    for var j in 0 ..< 200 do
      var th := new Thing(j);
      var g := th.Get;
      t := t + fl.M(g);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
