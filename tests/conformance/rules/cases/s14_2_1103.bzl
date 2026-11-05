//! case: s14_2_1103
//! doc: spec
//! section: 14.2
//! title: Min on a lazy sequence of a record with no IComparable is rejected
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
    Console.WriteLine($"{a.Where(lambda(x: P) => true).Min().v}");
  end Main;
end Program;
