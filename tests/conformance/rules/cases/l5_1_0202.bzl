//! case: l5_1_0202
//! doc: stdlib
//! section: 5.1
//! title: Min over a record that implements IComparable of another type only is rejected
//! rule: public extension IEnumerable<T> where T: IComparable<T>
//! cites: 5, 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 8092
module Conf;

public record B(Y: int);

public record A(X: int): IComparable<B>
  public method CompareTo(other: B): int => X - other.Y;
end A;

public class Program
  public static method Main()
    var a: Array<A> := [new A(3), new A(1)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
