//! case: l5_1_0801
//! doc: stdlib
//! section: 5.1
//! title: no-selector Min and Max over generic records order by the record's CompareTo, also when reversed
//! rule: `Min` and `Max` without a selector are generic over comparable types:
//! cites: 5.1, 2.8
//! expect: run
//! output: 3 1
//!   3 1
//!   3 1
//!   1 3 y 1.5
//!   1 3 r 9
//! issue: 8140
//! notes: W's CompareTo is reversed (a larger v compares less), so Min is the largest v. CompareTo negative means less (stdlib 2.8).
module Conf;

public record W<T>(v: int): IComparable<W<T>>
  public method CompareTo(other: W<T>): int => (if v > other.v then -1 else (if v < other.v then 1 else 0));
end W;

public record P<A, B>(a: A, v: int, b: B): IComparable<P<A, B>>
  public method CompareTo(other: P<A, B>): int => (if v > other.v then 1 else (if v < other.v then -1 else 0));
end P;

public class Program
  public static method Main()
    var a: Array<W<string>> := [new W<string>(3), new W<string>(1), new W<string>(2)];
    Console.WriteLine($"{a.Min().v} {a.Max().v}");
    var b: Array<W<int>> := [new W<int>(3), new W<int>(1), new W<int>(2)];
    Console.WriteLine($"{b.Min().v} {b.Max().v}");
    var c: Array<W<W<int>>> := [new W<W<int>>(3), new W<W<int>>(1), new W<W<int>>(2)];
    Console.WriteLine($"{c.Min().v} {c.Max().v}");
    var d: Array<P<string, double>> := [new P<string, double>("x", 3, 1.5), new P<string, double>("y", 1, 2.5), new P<string, double>("z", 2, 0.5)];
    Console.WriteLine($"{d.Min().v} {d.Max().v} {d.Min().a} {d.Max().b}");
    var e: Array<P<int, string>> := [new P<int, string>(9, 3, "q"), new P<int, string>(8, 1, "r"), new P<int, string>(7, 2, "s")];
    Console.WriteLine($"{e.Min().v} {e.Max().v} {e.Min().b} {e.Max().a}");
  end Main;
end Program;
