//! case: l5_1_1102
//! doc: stdlib
//! section: 5.1
//! title: Min, Max and the OrNull forms over a generic record IComparable<W<T>> at two instantiations
//! rule: `Min` and `Max` without a selector are generic over comparable types ... public method
//!   MinOrNull(): T?; ... null if empty
//! cites: 14.2
//! expect: run
//! output: b a
//!   b
//!   b a
//!   True
//!   9
//! issue: 8161
module Conf;

public record W<T>(v: int, t: T): IComparable<W<T>>
  public method CompareTo(other: W<T>): int => v - other.v;
end W;

public class Program
  public static method Main()
    var a: Array<W<string>> := [new W<string>(3, "a"), new W<string>(1, "b"), new W<string>(2, "c")];
    Console.WriteLine($"{a.Min().t} {a.Max().t}");
    Console.WriteLine($"{a.Where(lambda(x: W<string>) => x.v > 0).Min().t}");
    Console.WriteLine($"{a.MinOrNull()!.t} {a.MaxOrNull()!.t}");
    using var e := new Array<W<int>>();
    Console.WriteLine($"{e.MinOrNull() = null}");
    var b: Array<W<int>> := [new W<int>(5, 9)];
    Console.WriteLine($"{b.MinOrNull()!.t}");
  end Main;
end Program;
