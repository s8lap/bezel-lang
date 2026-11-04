//! case: l5_1_0802
//! doc: stdlib
//! section: 5.1
//! title: no-selector Min and Max over a generic record conforming via an extending interface use CompareTo
//! rule: `Min` and `Max` without a selector are generic over comparable types:
//! cites: 5.1, 14.2
//! expect: run
//! output: 1 3
//!   1 3
//! issue: 8140
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public record W<T>(v: int): IOrd<W<T>>
  public method CompareTo(other: W<T>): int => (if v > other.v then 1 else (if v < other.v then -1 else 0));
end W;

public class Program
  public static method Main()
    var a: Array<W<string>> := [new W<string>(3), new W<string>(1), new W<string>(2)];
    Console.WriteLine($"{a.Min().v} {a.Max().v}");
    var b: Array<W<int>> := [new W<int>(3), new W<int>(1)];
    Console.WriteLine($"{b.Min().v} {b.Max().v}");
  end Main;
end Program;
