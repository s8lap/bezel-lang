//! case: s28_9_4307
//! doc: spec
//! section: 28.9
//! title: boxed DateTime and TimeSpan values interleaved with a class instance in an Object field read back
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 40055
module Conf;

public class Widget
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
end Widget;

public class Holder
  public o: Object?;
  public constructor() end constructor;
  public method TakeDate(y: int)
    o := new DateTime(2000 + y, 1, 2);
  end TakeDate;
  public method TakeSpan(n: int)
    o := TimeSpan.FromSeconds(n);
  end TakeSpan;
  public method Put(x: Object)
    o := x;
  end Put;
  public method Year(): int
    return (o as DateTime)?.Year ?? -1;
  end Year;
end Holder;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<10 do
      using var w := new Widget(7);
      using var h := new Holder();
      h.TakeDate(i mod 10);
      sum += h.Year();
      h.Put(w);
      h.TakeSpan(i);
      h.TakeDate(1);
      sum += h.Year();
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
