//! case: l1_2301
//! doc: stdlib
//! section: 1
//! title: the composed Where(pred).Count() on a SortedSet reaches the
//!   operator pipeline
//! rule: the composed spelling — `coll.Where(pred).Count()`; `Where` is
//!   shadowed nowhere in this document — reaches the operator pipeline
//! cites: 5, 26.5
//! expect: run
//! output: 2
//! notes: Accepting twin of s26_5_2301 (green on base and tip), Where
//!   returns an IEnumerable<int>, which declares no Count member.
module Conf;

public class Program
  public static method Main()
    var s: SortedSet<int> := new SortedSet<int>();
    s.Add(1);
    s.Add(2);
    s.Add(3);
    Console.WriteLine(s.Where(lambda(x: int) => x > 1).Count());
  end Main;
end Program;
