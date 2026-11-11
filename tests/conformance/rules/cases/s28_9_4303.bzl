//! case: s28_9_4303
//! doc: spec
//! section: 28.9
//! title: one box shared by two holders stays intact when either holder's field is overwritten
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 90
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
  public method CopyFrom(other: Holder)
    o := other.o;
  end CopyFrom;
  public method Read(): int
    return (o as int) ?? -1;
  end Read;
end Holder;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<10 do
      using var a := new Holder();
      using var b := new Holder();
      a.Take(i);
      b.CopyFrom(a);
      a.Take(1000);
      sum += b.Read();
      a.CopyFrom(b);
      b.Take(0);
      sum += a.Read();
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
