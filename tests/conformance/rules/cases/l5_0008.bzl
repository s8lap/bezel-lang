//! case: l5_0008
//! doc: stdlib
//! section: 5
//! title: an OrderBy result mints a single-consumption frame by its lazy-query kind
//! rule: Each deferred operator call mints a caller-owned query frame; where
//!   the declared type is one of stdlib section 5's derived interfaces
//!   instead (the OrderBy/OrderByDescending/ThenBy/ThenByDescending
//!   quartet's IOrderedEnumerable<T>), the operator is the same minting
//!   member by its lazy-query kind.
//! cites: 5, 29.11
//! expect: reject
//! diagnostic: single-consumption
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [3, 1, 2];
    using var q := xs.OrderBy(lambda(x: int): int => x);
    for var v in q do
      Console.WriteLine(v);
    end;
    for var v2 in q do
      Console.WriteLine(v2);
    end;
  end Main;
end Program;
