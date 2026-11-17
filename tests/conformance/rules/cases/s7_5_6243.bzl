//! case: s7_5_6243
//! doc: spec
//! section: 7.5
//! title: ?.Sort with a method-group comparer on a non-null receiver sorts
//!   in place
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access.
//! cites: 7.5, 4.4
//! expect: run
//! stdout: a,b,c
module Conf;

record W(K: string, V: int);

public class Program
  static method Cmp(x: W, y: W): int
    return x.K.CompareTo(y.K);
  end Cmp;

  public static method Main()
    var a: Array<W> := [new W("b", 1), new W("c", 2), new W("a", 3)];
    var n: Array<W>? := a;
    n?.Sort(Cmp);
    Console.WriteLine($"{a[0].K},{a[1].K},{a[2].K}");
  end Main;
end Program;
