//! case: s28_3_6451
//! doc: spec
//! section: 28.3
//! title: a borrowed Array parameter returned as IEnumerable stays the caller's and
//!   is enumerated
//! rule: Parameter `p` (no modifier) | Borrowed (source: caller)
//! cites: 28.3, 28.3.4
//! expect: run
//! stdout: 1800
module Conf;
public class Consume
  public static method Total(e: IEnumerable<int>): int
    var s := 0;
    for var x in e do s := s + x; end;
    return s;
  end Total;
  public static method F(a: Array<int>): IEnumerable<int>
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var arr: Array<int> := [1, 2, 3];
      var u := Consume.F(arr);
      s := s + Consume.Total(u);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
