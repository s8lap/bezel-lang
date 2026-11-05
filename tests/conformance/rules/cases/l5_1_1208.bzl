//! case: l5_1_1208
//! doc: stdlib
//! section: 5.1
//! title:Min() is rejected on Box of string whose IComparable names Box of int
//! rule:public extension IEnumerable<T> where T: IComparable<T>
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
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
    var a: Array<Box<string>> := [new Box<string>(3), new Box<string>(1)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
