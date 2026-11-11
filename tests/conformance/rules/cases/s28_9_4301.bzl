//! case: s28_9_4301
//! doc: spec
//! section: 28.9
//! title: a boxed int in an Object field, a class instance and another box replace each other undisturbed
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 115
module Conf;

public class Holder
  public o: Object?;
  public constructor() end constructor;
  public method Take(n: int)
    o := n;
  end Take;
  public method Put(x: Object)
    o := x;
  end Put;
  public method Read(): int
    return (o as int) ?? -1;
  end Read;
end Holder;

public class Widget
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
  public method Get(): int => v;
end Widget;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<10 do
      using var w := new Widget(7);
      using var h := new Holder();
      h.Put(w);
      h.Take(i);
      sum += h.Read();
      h.Put(w);
      sum += w.Get();
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
