//! case: s28_9_9603
//! doc: spec
//! section: 28.9
//! title: a boxed value passed to a method reference over a sealed override is usable afterwards
//! rule: so a boxed value is an immutable shared reference
//! cites: 28.9, 8.6, 27.6
//! expect: run
//! stdout: 190
//! issue: 9206
module Conf;

public class Base
  public constructor()
  end constructor;

  public virtual method Look(o: Object): int => 0;
end Base;

public class Derived: Base
  public constructor()
  end constructor;

  public sealed override method Look(o: Object): int => o.ToString().Length;
end Derived;

public class Program
  public static method Main()
    var d := new Derived();
    var f: (Object) -> int := d.Look;
    var t := 0;
    for var i in 0 ..< 100 do
      var b: Object := i;
      var n := f(b);
      t := t + n;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
