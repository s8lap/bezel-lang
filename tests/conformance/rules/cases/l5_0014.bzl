//! case: l5_0014
//! doc: stdlib
//! section: 5
//! title: GroupBy keyed by a function type is a compile error
//! rule: GroupBy states `where TKey: hashable` as an ordinary member-level
//!   where clause on its own type parameter, the ordinary constraint form of
//!   language spec 14.2 -- the same requirement ToDictionary states on its
//!   TKey. A function type is not hashable (spec 3.6 clause (e)), so a key
//!   selector returning one fails the constraint at the call site.
//! cites: 5, 14.2, 3.6 (hashable row clause (e)), 27.7.1
//! expect: reject
//! diagnostic: where TKey: hashable
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    var groups := xs.GroupBy(lambda(x: int): (int) -> int => Program.Key2).Count();
    Console.WriteLine(groups);
  end Main;

  public static method Key2(n: int): int => n * 2;
end Program;