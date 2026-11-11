//! case: s28_9_4610
//! doc: spec
//! section: 28.9
//! title: an Object field value passed to a `?.` call keeps its box when the callee overwrites the field
//! rule: so a boxed value is an immutable shared reference
//! cites: 8.6, 7.5
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
      var hh: Holder? := h;
      sum += hh?.UseSelf(h.o) ?? -7;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
