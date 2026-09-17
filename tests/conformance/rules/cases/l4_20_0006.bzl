//! case: l4_20_0006
//! doc: stdlib
//! section: 4.20
//! title: Increment on an existing key updates in place and preserves its insertion position
//! rule: Increment on an existing key updates in place and preserves its
//!   position.
//! cites: 4.20
//! expect: run
//! output: a
//!   b
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    tally.Increment("a");
    tally.Increment("b");
    tally.Increment("a");
    for var e in tally do
      Console.WriteLine(e.Key);
    end;
  end Main;
end Program;
