//! case: s14_2_1104
//! doc: spec
//! section: 14.2
//! title: MinOrNull over a record with no IComparable is rejected
//! rule: **T is a record** that lists I (or any interface extending I) in its inheritance clause
//!   and declares matching members
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 8161
module Conf;

public record P(v: int);

public class Program
  public static method Main()
    var a: Array<P> := [new P(1)];
    Console.WriteLine($"{a.MinOrNull()!.v}");
  end Main;
end Program;
