//! case: s35_7_0003
//! doc: spec
//! section: 35.7
//! title: comparing a non-auto-hashable owning choice without declared operators is a use-site error
//! rule: A choice that is not auto-hashable — any borrow-bearing or owning choice —
//!   draws a use-site compile error on =, <>, and direct Equals/GetHashCode calls,
//!   unless the author declares the operators under §25.2's co-declaration rule.
//! expect: reject
//! diagnostic: Equals
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public class Program
  public static method Main()
    using var a := Outcome.Fetched(new Widget());
    using var b := Outcome.Fetched(new Widget());
    Console.WriteLine(a = b);    // ERROR: Outcome is owning and declares no equality operators
  end Main;
end Program;
