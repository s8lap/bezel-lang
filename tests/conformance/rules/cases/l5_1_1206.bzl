//! case: l5_1_1206
//! doc: stdlib
//! section: 5.1
//! title:Min() is admitted when the element type implements IComparable of the same instantiation
//! rule:public extension IEnumerable<T> where T: IComparable<T>
//! cites: 14.2
//! expect: run
//! stdout: 1
module Conf;

public class Box<T>: IComparable<Box<int>>
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;

  public method CompareTo(o: Box<int>): int => X - o.X;
end Box;

public class Program
  public static method Main()
    var a: Array<Box<int>> := [new Box<int>(3), new Box<int>(1)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
