//! case: s28_9_4306
//! doc: spec
//! section: 28.9
//! title: a box stored through a null-conditional call on a nullable holder reads back
//! rule: so a boxed value is an immutable shared reference.
//! cites: 28.9, 8.6, 7.5
//! expect: run
//! stdout: 45
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
    var sum := 0;
    for var i in 0..<10 do
      using var h := new Holder();
      var hn: Holder? := h;
      hn?.Take(i);
      sum += hn?.Read() ?? -5;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
