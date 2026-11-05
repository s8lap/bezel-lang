//! case: l5_1_1205
//! doc: stdlib
//! section: 5.1
//! title:no-selector Min/Max over a generic class implementing IComparable of itself return the
//!   element
//! rule:`Min` and `Max` without a selector are generic over comparable types:
//! cites: 5, 14.2
//! expect: run
//! output: 1
//!   3
//!   2
//!   1
module Conf;

public class Box<T>: IComparable<Box<T>>
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;

  public method CompareTo(o: Box<T>): int => X - o.X;
end Box;

public class Program
  public static method Main()
    var a: Array<Box<int>> := [new Box<int>(3), new Box<int>(1), new Box<int>(2)];
    Console.WriteLine(a.Min().X);
    Console.WriteLine(a.Max().X);
    Console.WriteLine(a.Where(lambda(x: Box<int>) => x.X > 1).Min().X);
    Console.WriteLine(a.MinOrNull()?.X ?? -1);
  end Main;
end Program;
