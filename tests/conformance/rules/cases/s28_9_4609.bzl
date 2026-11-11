//! case: s28_9_4609
//! doc: spec
//! section: 28.9
//! title: an Object alias taken through a conditional expression keeps its box when the field is overwritten
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
      using var g := new Holder();
      h.Take(i);
      g.Take(i);
      var t: Object? := (if i mod 2 = 0 then h.o else g.o);
      h.Take(5000);
      g.Take(5000);
      sum += (t as int) ?? -1;
    end for;
    Console.WriteLine(sum);
  end Main;
end Program;
