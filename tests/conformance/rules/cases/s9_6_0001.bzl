//! case: s9_6_0001
//! doc: spec
//! section: 9.6
//! title: a record may implement an interface and provide the matching member
//! rule: A record may implement any number of interfaces that declare no
//!   settable property or indexer member.
//! expect: run
//! stdout: -1
module Conf;

public record Point(X: int, Y: int): IComparable<Point>
  public method CompareTo(other: Point): int =>
    (if X <> other.X then (if X < other.X then -1 else 1)
     elif Y <> other.Y then (if Y < other.Y then -1 else 1)
     else 0);
end Point;

public class Program
  public static method Main()
    var a := new Point(1, 2);
    var b := new Point(1, 5);
    Console.WriteLine(a.CompareTo(b));
  end Main;
end Program;
