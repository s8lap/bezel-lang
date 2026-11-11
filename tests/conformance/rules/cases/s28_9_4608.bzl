//! case: s28_9_4608
//! doc: spec
//! section: 28.9
//! title: an Object alias passed to a constructor still reads its box after the field is overwritten
//! rule: so a boxed value is an immutable shared reference
//! cites: 8.6
//! expect: run
//! stdout: 19900
module Conf;

public class Holder
  public o: Object?;
  public constructor() end constructor;
  public method Take(n: int) o := n; end Take;
  public method UseSelf(x: Object?): int
    Take(5000);
    return (x as int) ?? -1;
  end UseSelf;
end Holder;
public class W
  public r: int;
  public constructor(x: Object?, h: Holder)
    h.Take(5000);
    r := (x as int) ?? -1;
  end constructor;
end W;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<200 do
      using var h := new Holder();
      h.Take(i);
      using var w := new W(h.o, h);
      sum += w.r;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
