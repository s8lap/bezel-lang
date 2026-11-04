//! case: l5_1_0203
//! doc: stdlib
//! section: 5.1
//! title: Min and MaxOrNull over a record whose second base is IComparable of itself are admitted
//! rule: public extension IEnumerable<T> where T: IComparable<T>
//! cites: 5, 14.2
//! expect: run
//! output: 1
//!   3
//! issue: 8092
module Conf;

public interface IFoo
  method Foo(): int;
end IFoo;

public record Pt(X: int): IFoo, IComparable<Pt>
  public method Foo(): int => X;
  public method CompareTo(other: Pt): int => (if X > other.X then 1 else (if X < other.X then -1 else 0));
end Pt;

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3), new Pt(1)];
    Console.WriteLine(a.Min().X);
    Console.WriteLine(a.MaxOrNull()!.X);
  end Main;
end Program;
