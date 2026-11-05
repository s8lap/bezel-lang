//! case: s14_2_1102
//! doc: spec
//! section: 14.2
//! title: MinOrNull over W<string> is rejected when W<T> lists only IComparable<W<int>>
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause
//!   and declares matching members
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 8161
module Conf;

public record W<T>(v: int): IComparable<W<int>>
  public method CompareTo(other: W<int>): int => v - other.v;
end W;

public class Program
  public static method Main()
    var a: Array<W<string>> := [new W<string>(3)];
    Console.WriteLine($"{a.MinOrNull()!.v}");
  end Main;
end Program;
