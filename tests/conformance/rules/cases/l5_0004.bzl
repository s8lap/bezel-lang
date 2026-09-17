//! case: l5_0004
//! doc: stdlib
//! section: 5
//! title: composing a further deferred operator spends the source query's enumeration right
//! rule: Applying a further deferred operator (var q2 := q.Select(f);)
//!   spends the binding q's enumeration right -- using q afterwards as a
//!   query is a static error where visible.
//! cites: 5, 29.11
//! expect: reject
//! diagnostic: single-consumption
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x: int): bool => x > 0);
    using var q2 := q.Select(lambda(x: int): int => x * 2);
    for var v in q do
      Console.WriteLine(v);
    end;
  end Main;
end Program;
