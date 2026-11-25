//! case: s28_9_9606
//! doc: spec
//! section: 28.9
//! title: a boxed value forwarded by a lambda to a method that stores it stays readable
//! rule: so a boxed value is an immutable shared reference
//! cites: 28.9, 8.6
//! expect: run
//! output: 100
//!   99
//! issue: 9206
module Conf;

public class Keeper
  public Last: Object? := null;

  public constructor()
  end constructor;

  public method Keep(o: Object): int
    this.Last := o;
    return 1;
  end Keep;
end Keeper;

public class Program
  public static method Main()
    var k := new Keeper();
    var f := lambda(x: Object) => k.Keep(x);
    var t := 0;
    for var i in 0 ..< 100 do
      var b: Object := i;
      var n := f(b);
      t := t + n;
    end for;
    Console.WriteLine(t);
    var o := k.Last;
    if o <> null then
      Console.WriteLine(o.ToString());
    end if;
  end Main;
end Program;
