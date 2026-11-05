//! case: s22_6_2310
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array at an IReadOnlyCollection slot
//!   iterates the elements twice
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3, 4.1
//! expect: run
//! output: 12
//! notes: Accepting control (green on base and tip).
module Conf;

public class H
  public static method Run(xs: Array<int>, f: (Array<int>) -> IReadOnlyCollection<int>): int
    var c := f(xs);
    var n := 0;
    for var v in c do n := n + v; end;
    for var v in c do n := n + v; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(H.Run(xs, lambda(l) => l));
  end Main;
end Program;
