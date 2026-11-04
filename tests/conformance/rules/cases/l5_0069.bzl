//! case: l5_0069
//! doc: stdlib
//! section: 5
//! title: Skip, Take, OrderBy and Distinct feed Min, First, Last and Contains over an IList-typed receiver
//! rule: public method Take(count: int): IEnumerable<T>;
//! cites: 4.1, 5
//! expect: run
//! output: 1
//!   5
//!   8
//!   True
//! issue: 7625
//! notes: twin of l5_0068 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    Console.WriteLine(l.Skip(1).Min());
    Console.WriteLine(l.Take(2).First());
    Console.WriteLine(l.OrderBy(lambda(x: int): int => x).Last());
    Console.WriteLine(l.Distinct().Contains(3));
  end Main;
end Program;
