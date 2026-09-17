//! case: s17_6_0009
//! doc: spec
//! section: 17.6
//! title: ++ on a read-only property is rejected
//! rule: "The operand must be an assignable lvalue (a local variable, a
//!   parameter, a writable field, an indexer slot, or a property with a
//!   setter) ... Read-only locations cannot be incremented."
//! cites: 17.6, 23.2
//! expect: reject
//! diagnostic: read-only
module Conf;

public class Gauge
  public constructor()
  end constructor;

  public property Level: int => 3;
end Gauge;

public class Program
  public static method Main()
    using var g := new Gauge();
    g.Level++;
  end Main;
end Program;
