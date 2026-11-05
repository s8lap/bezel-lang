//! case: s22_6_2303
//! doc: spec
//! section: 22.6
//! title: a lambda returning OrderBy, Take and Distinct pipelines at an
//!   IEnumerable slot keeps their order
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3, 5
//! expect: run
//! output: 466
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(xs: Array<int>, f: (Array<int>) -> IEnumerable<int>): int
    var n := 0;
    for var v in f(xs) do n := n * 10 + v; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [3, 1, 2];
    var t := H.Run(xs, lambda(l) => l.OrderBy(lambda(x) => x));
    t := t + H.Run(xs, lambda(l) => l.Take(2));
    t := t + H.Run(xs, lambda(l) => l.Distinct());
    Console.WriteLine(t);
  end Main;
end Program;
