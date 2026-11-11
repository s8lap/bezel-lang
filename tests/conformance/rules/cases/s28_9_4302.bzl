//! case: s28_9_4302
//! doc: spec
//! section: 28.9
//! title: a boxed int replaced by null and by another box reads back the latest box
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 25
module Conf;

public class Holder
  public o: Object?;
  public constructor() end constructor;
  public method Take(n: int)
    o := n;
  end Take;
  public method Put(x: Object?)
    o := x;
  end Put;
  public method Clear()
    o := null;
  end Clear;
  public method Read(): int
    return (o as int) ?? -1;
  end Read;
end Holder;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<10 do
      using var h := new Holder();
      h.Take(i);
      h.Clear();
      sum += h.Read();
      h.Take(i);
      h.Put(null);
      sum += h.Read();
      h.Take(i);
      sum += h.Read();
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
