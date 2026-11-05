//! case: l5_1_1209
//! doc: stdlib
//! section: 5.1
//! title:Min() is rejected on a derived class whose base alone implements IComparable of the base
//! rule:public extension IEnumerable<T> where T: IComparable<T>
//! cites: 14.2, 12.9
//! expect: reject
//! diagnostic: IComparable
module Conf;

public class A: IComparable<A>
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;

  public method CompareTo(o: A): int => X - o.X;
end A;

public class B: A
  public constructor(x: int)
    base(x);
  end constructor;
end B;

public class Program
  public static method Main()
    var a: Array<B> := [new B(3), new B(1), new B(2)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
