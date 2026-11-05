//! case: s22_6_2308
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array at a generic IEnumerable<U> slot
//!   iterates
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 14.5, 27.3
//! expect: run
//! output: 3
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
//!   Generic-method form (probe p24 of the #8168 review).
module Conf;

public class C
  public static method Cnt<U>(xs: Array<U>, f: (Array<U>) -> IEnumerable<U>): int
    var n := 0;
    for var v in f(xs) do n := n + 1; end;
    return n;
  end Cnt;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(Cnt(xs, lambda(l) => l));
  end Main;
end C;
