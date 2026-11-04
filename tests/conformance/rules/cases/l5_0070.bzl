//! case: l5_0070
//! doc: stdlib
//! section: 5
//! title: Aggregate, Single and ElementAt consume deferred operators over an IList-typed receiver
//! rule: public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum):
//!   TAccum;
//! cites: 4.1, 5
//! expect: run
//! output: 17
//!   8
//!   4
//! issue: 7625
//! notes: twin of l5_0068 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    Console.WriteLine(l.Select(lambda(x: int): int => x).Aggregate(0, lambda(acc: int, x: int): int => acc + x));
    Console.WriteLine(l.Where(lambda(x: int): bool => x > 1).Single(lambda(x: int): bool => x = 8));
    Console.WriteLine(l.Select(lambda(x: int): int => x + 1).ElementAt(1));
  end Main;
end Program;
