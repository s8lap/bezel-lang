//! case: l5_1_0204
//! doc: stdlib
//! section: 5.1
//! title: Min over a record that implements IComparable of both another type and itself is admitted
//! rule: public extension IEnumerable<T> where T: IComparable<T>
//! cites: 5, 14.2
//! expect: run
//! output: 1
//! issue: 8092
module Conf;

public record B(Y: int);

public record A(X: int): IComparable<B>, IComparable<A>
  public method CompareTo(other: A): int => (if X > other.X then 1 else (if X < other.X then -1 else 0));
  public method CompareTo(other: B): int => 0;
end A;

public class Program
  public static method Main()
    var a: Array<A> := [new A(3), new A(1)];
    Console.WriteLine(a.Min().X);
  end Main;
end Program;
