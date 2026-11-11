//! case: s14_5_4814
//! doc: spec
//! section: 14.5
//! title: an sbyte and a short range of negative bounds bind T to the signed element width
//! rule: or against the unique instantiation of `G` that the argument's type implements or extends
//! cites: 14.5, stdlib 9.8
//! expect: run
//! output: -3
//!   -5
//!   -897
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
    var a: sbyte := -3;
    var b: sbyte := 2;
    Console.WriteLine(Sum(a..b, lambda(v) => long(v)));
    Console.WriteLine(Sum(a..<b, lambda(v) => long(v)));
    var s: short := -300;
    var t: short := -298;
    Console.WriteLine(Sum(s..t, lambda(v) => long(v)));
  end Main;
end Program;
