//! case: s22_6_2304
//! doc: spec
//! section: 22.6
//! title: a block lambda with two returns converts each return to the
//!   IEnumerable slot
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3
//! expect: run
//! output: 11
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(xs: Array<int>, b: bool, f: (Array<int>, bool) -> IEnumerable<int>): int
    var n := 0;
    for var v in f(xs, b) do n := n + v; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var t := 0;
    t := t + H.Run(xs, true, lambda(l, b) => do
      if b then return l; end;
      return l.Where(lambda(x) => x > 1);
    end);
    t := t + H.Run(xs, false, lambda(l, b) => do
      if b then return l; end;
      return l.Where(lambda(x) => x > 1);
    end);
    Console.WriteLine(t);
  end Main;
end Program;
