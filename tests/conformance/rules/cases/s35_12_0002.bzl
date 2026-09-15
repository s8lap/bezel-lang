//! case: s35_12_0002
//! doc: spec
//! section: 35.12
//! title: an owning choice is not directly sendable
//! rule: Owning choices are not directly sendable — the §30.14 move-only exclusion,
//!   unchanged.
//! expect: reject
//! diagnostic: sendable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public class Checker<T> where T: sendable
  public static method Check(): bool => true;
end Checker;

public class Program
  public static method Main()
    Console.WriteLine(Checker<Outcome>.Check());   // ERROR: an owning choice is not directly sendable
  end Main;
end Program;
