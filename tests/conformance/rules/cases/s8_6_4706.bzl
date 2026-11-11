//! case: s8_6_4706
//! doc: spec
//! section: 8.6
//! title: boxed Guid values built from a narrowed Guid? in a loop render and compare correctly
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 7.4, 28.9
//! expect: run
//! stdout: 21600
module Conf;

public class Program
  public static method TakeO(o: Object): int
    return o.ToString().Length;
  end TakeO;

  public static method Main()
    var total := 0;
    var i := 0;
    while i < 300 do
      var n: Guid? := Guid.Empty;
      var g: IComparable<Guid> := n;
      var o: Object := n;
      total := total + g.CompareTo(Guid.Empty) + o.ToString().Length + Program.TakeO(n);
      i := i + 1;
    end while;
    Console.WriteLine(total);
  end Main;
end Program;
