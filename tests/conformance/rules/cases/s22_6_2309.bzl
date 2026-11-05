//! case: s22_6_2309
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array at an Array return slot is unchanged
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3
//! expect: run
//! output: 12
//! notes: Accepting control (green on base and tip).
module Conf;

public class H
  public static method Run(xs: Array<int>, f: (Array<int>) -> Array<int>): int
    var n := 0;
    for var v in f(xs) do n := n + v; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var t := H.Run(xs, lambda(l) => l);
    t := t + H.Run(xs, lambda(l: Array<int>): Array<int> => l);
    Console.WriteLine(t);
  end Main;
end Program;
