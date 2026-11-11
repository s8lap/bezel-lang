//! case: s14_5_4816
//! doc: spec
//! section: 14.5
//! title: a range at IEnumerable<T> beside a seed and a two-parameter lambda binds T and R independently
//! rule: or against the unique instantiation of `G` that the argument's type implements or extends
//! cites: 14.5, stdlib 9.8
//! expect: run
//! output: 150
//!   10000000001
//!   131069
module Conf;

public class Program
  private static method Fold<T, R>(xs: IEnumerable<T>, seed: R, f: (R, T) -> R): R
    var acc := seed;
    for var x in xs do
      acc := f(acc, x);
    end;
    return acc;
  end Fold;

  public static method Main()
    var a: byte := 1;
    var b: byte := 5;
    var k := 10L;
    Console.WriteLine(Fold(a..b, 0L, lambda(acc, v) => acc + long(v) * k));
    var l: long := 5000000000;
    var m: long := 5000000001;
    Console.WriteLine(Fold(l..m, 0L, lambda(acc, v) => acc + v));
    var u: ushort := 65534;
    var uh: ushort := 65535;
    Console.WriteLine(Fold(u..uh, 0L, lambda(acc, v) => acc + long(v)));
  end Main;
end Program;
