//! case: s14_5_4815
//! doc: spec
//! section: 14.5
//! title: a range forwarded through a generic wrapper keeps its element type: byte, int and long ranges
//! rule: or against the unique instantiation of `G` that the argument's type implements or extends
//! cites: 14.5, stdlib 9.8
//! expect: run
//! output: 9
//!   9
//!   5
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

  private static method Outer<U>(xs: IEnumerable<U>): int
    return Cnt(xs);
  end Outer;

  public static method Main()
    var a: byte := 1;
    var b: byte := 9;
    Console.WriteLine(Outer(a..b));
    Console.WriteLine(Outer(1..9));
    var l: long := 5000000000;
    var m: long := 5000000004;
    Console.WriteLine(Outer(l..m));
  end Main;
end Program;
