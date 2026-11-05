//! case: s14_2_0907
//! doc: spec
//! section: 14.2
//! title: Math.Max<V> as method reference and call under V: IComparable<V>, at int, string and uint
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule
//! cites: 14.5, 27.6
//! expect: run
//! output: -3
//!   2
//!   b
//!   4000000000
//! issue: 6295
module Conf;

public class Helper
  public static method F<V>(): (V, V) -> V where V: IComparable<V>
    return Math.Max<V>;
  end F;

  public static method G<V>(a: V, b: V): V where V: IComparable<V>
    return Math.Max<V>(a, b);
  end G;
end Helper;

public class Program
  public static method Main()
    var f: (int, int) -> int := Helper.F<int>();
    Console.WriteLine(f(-3, -5));
    Console.WriteLine(Helper.G<int>(-1, 2));
    var s: (string, string) -> string := Helper.F<string>();
    Console.WriteLine(s("b", "a"));
    var u: (uint, uint) -> uint := Helper.F<uint>();
    Console.WriteLine(u(4000000000u, 3u));
  end Main;
end Program;
