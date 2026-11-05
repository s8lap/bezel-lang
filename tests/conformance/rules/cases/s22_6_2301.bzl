//! case: s22_6_2301
//! doc: spec
//! section: 22.6
//! title: a lambda returning its Array parameter at an IEnumerable return
//!   slot iterates the elements
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3, 29.2
//! expect: run
//! output: 12
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(xs: Array<int>, f: (Array<int>) -> IEnumerable<int>): int
    var n := 0;
    for var v in f(xs) do n := n + v; end;
    for var v in f(xs) do n := n + v; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(H.Run(xs, lambda(l) => l));
  end Main;
end Program;
