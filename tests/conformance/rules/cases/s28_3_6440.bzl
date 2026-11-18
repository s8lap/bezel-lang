//! case: s28_3_6440
//! doc: spec
//! section: 28.3
//! title: a literal-initialised Array local returned as IEnumerable is enumerated
//!   twice by the caller
//! rule: Iterating the same source twice with two `for` loops works as expected for
//!   collection types
//! cites: 28.3, 29.11
//! expect: run
//! output: 3600
module Conf;
public class Consume
  public static method Total(e: IEnumerable<int>): int
    var s := 0;
    for var x in e do s := s + x; end;
    return s;
  end Total;
  public static method F(): IEnumerable<int>
    var a: Array<int> := [1, 2, 3];
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      s := s + Consume.Total(u);
      s := s + Consume.Total(u);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
