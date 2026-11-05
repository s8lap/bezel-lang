//! case: s22_6_2311
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array at an IReadOnlyList slot supports
//!   indexing and Count
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3, 4.1
//! expect: run
//! output: 5
//! notes: Accepting control (green on base and tip).
module Conf;

public class H
  public static method Run(xs: Array<int>, f: (Array<int>) -> IReadOnlyList<int>): int
    var c := f(xs);
    return c[1] + c.Count;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(H.Run(xs, lambda(l) => l));
  end Main;
end Program;
