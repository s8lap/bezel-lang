//! case: l5_1_1211
//! doc: stdlib
//! section: 5.1
//! title:Min() on a lazy Where over a class with no ordering is rejected
//! rule:public extension IEnumerable<T> where T: IComparable<T>
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
module Conf;

public class N
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;
end N;

public class Program
  public static method Main()
    var a: Array<N> := [new N(3), new N(1)];
    Console.WriteLine(a.Where(lambda(x: N) => true).Min().X);
  end Main;
end Program;
