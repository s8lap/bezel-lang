//! case: l5_1_1204
//! doc: stdlib
//! section: 5.1
//! title:no-selector Min/Max over a class element type ordered by its own CompareTo return the
//!   element
//! rule:`Min` and `Max` without a selector are generic over comparable types:
//! cites: 5, 2.8
//! expect: run
//! output: 2
//!   4
//!   7
//!   7
//!   7
module Conf;

public class K: IComparable<K>
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;

  public method CompareTo(o: K): int => X - o.X;
end K;

public class Program
  public static method Main()
    var l: Array<K> := [new K(4), new K(2), new K(7)];
    Console.WriteLine(l.Min().X);
    Console.WriteLine(l.Where(lambda(k: K) => k.X > 2).Min().X);
    Console.WriteLine(l.OrderBy(lambda(k: K) => k.X).Max().X);
    Console.WriteLine(l.Select(lambda(k: K) => k).Max().X);
    Console.WriteLine(l.Where(lambda(k: K) => k.X > 2).MaxOrNull()?.X ?? -1);
  end Main;
end Program;
