//! case: l5_1_1104
//! doc: stdlib
//! section: 5.1
//! title: Min, lazy Max and MinOrNull over W<int> with W<T>: IComparable<W<int>> are accepted
//! rule: `Min` and `Max` without a selector are generic over comparable types ... public method
//!   MinOrNull(): T?;
//! cites: 14.2
//! expect: run
//! output: 1 3 1
//! issue: 8161
module Conf;

public record W<T>(v: int): IComparable<W<int>>
  public method CompareTo(other: W<int>): int => v - other.v;
end W;

public class Program
  public static method Main()
    var a: Array<W<int>> := [new W<int>(3), new W<int>(1)];
    Console.WriteLine($"{a.Min().v} {a.Where(lambda(x: W<int>) => true).Max().v} {a.MinOrNull()!.v}");
  end Main;
end Program;
