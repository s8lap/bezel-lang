//! case: s28_3_6450
//! doc: spec
//! section: 28.3
//! title: an Array local initialised from an owned-returning call and returned as
//!   IEnumerable is enumerated
//! rule: `new`, reference-typed collection literals, `owned`-returning calls,
//!   `move` results
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 1800
module Conf;
public class Consume
  public static method Total(e: IEnumerable<int>): int
    var s := 0;
    for var x in e do s := s + x; end;
    return s;
  end Total;
  public static method G(): owned Array<int>
    var a: Array<int> := [1, 2, 3];
    return a;
  end G;
  public static method F(): IEnumerable<int>
    var a: Array<int> := G();
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      s := s + Consume.Total(u);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
