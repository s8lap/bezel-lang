//! case: s14_5_4813
//! doc: spec
//! section: 14.5
//! title: a ushort and a byte range passed at IEnumerable<T> bind T to the range's own element width
//! rule: or against the unique instantiation of `G` that the argument's type implements or extends
//! cites: 14.5, 5.1, stdlib 9.8
//! expect: run
//! output: 196602
//!   3
//!   762
//!   3
module Conf;

public class Program
  private static method Sum<T>(xs: IEnumerable<T>, f: (T) -> long): long
    var n: long := 0;
    for var x in xs do
      n := n + f(x);
    end;
    return n;
  end Sum;

  private static method Cnt<T>(xs: IEnumerable<T>): int
    var n := 0;
    for var x in xs do
      n++;
    end;
    return n;
  end Cnt;

  public static method Main()
    var a: ushort := 65533;
    var b: ushort := 65535;
    Console.WriteLine(Sum(a..b, lambda(v) => long(v)));
    Console.WriteLine(Cnt(a..b));
    var c: byte := 253;
    var d: byte := 255;
    Console.WriteLine(Sum(c..d, lambda(v) => long(v)));
    Console.WriteLine(Cnt(c..d));
  end Main;
end Program;
