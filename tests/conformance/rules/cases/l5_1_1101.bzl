//! case: l5_1_1101
//! doc: stdlib
//! section: 5.1
//! title: Min, Max and the OrNull forms over a record element type implementing IComparable, also after Where
//! rule: `Min` and `Max` without a selector are generic over comparable types ... public method
//!   MinOrNull(): T?; ... null if empty
//! cites: 14.2
//! expect: run
//! output: 1 3
//!   2 2
//!   1 3
//!   True
//! issue: 8161
module Conf;

public record R(v: int): IComparable<R>
  public method CompareTo(other: R): int => v - other.v;
end R;

public class Program
  public static method Main()
    var a: Array<R> := [new R(3), new R(1), new R(1), new R(2)];
    Console.WriteLine($"{a.Min().v} {a.Max().v}");
    Console.WriteLine($"{a.Where(lambda(x: R) => x.v > 1).Min().v} {a.Where(lambda(x: R) => x.v < 3).Max().v}");
    Console.WriteLine($"{a.MinOrNull()!.v} {a.MaxOrNull()!.v}");
    using var e := new Array<R>();
    Console.WriteLine($"{e.MinOrNull() = null}");
  end Main;
end Program;
