//! case: l5_1_1207
//! doc: stdlib
//! section: 5.1
//! title:Min() is rejected on a class whose IComparable is at another type argument (IComparable of
//!   int)
//! rule:public extension IEnumerable<T> where T: IComparable<T>
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
module Conf;

public interface IW: IComparable<int>
end IW;

public class K: IW
  public X: int;

  public constructor(x: int)
    X := x;
  end constructor;

  public method CompareTo(o: int): int => X - o;
end K;

public class Program
  public static method Main()
    var a: Array<K> := [new K(2), new K(1)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
