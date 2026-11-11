//! case: s28_9_4611
//! doc: spec
//! section: 28.9
//! title: a const Object alias of a boxed field keeps its box when the field is overwritten
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

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<200 do
      using var h := new Holder();
      h.Take(i);
      const t := h.o;
      h.Take(5000);
      sum += (t as int) ?? -1;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
