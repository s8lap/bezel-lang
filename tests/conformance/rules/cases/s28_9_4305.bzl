//! case: s28_9_4305
//! doc: spec
//! section: 28.9
//! title: an Object root instance stored after a box in an Object field is not disturbed
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: ok
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

public class Program
  public static method Main()
    for var i in 0..<10 do
      using var r := new Object();
      using var h := new Holder();
      h.Take(i);
      h.Put(r);
    end for;
    Console.WriteLine("ok");
  end Main;
end Program;
