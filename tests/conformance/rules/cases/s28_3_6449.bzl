//! case: s28_3_6449
//! doc: spec
//! section: 28.3
//! title: a new-initialised Array local returned as IEnumerable is enumerated by the
//!   caller
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
  public static method F(): IEnumerable<int>
    var a: Array<int> := new Array<int>();
    a.Add(1); a.Add(2); a.Add(3);
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
