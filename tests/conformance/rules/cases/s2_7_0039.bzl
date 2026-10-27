//! case: s2_7_0039
//! doc: spec
//! section: 2.7
//! title: a chain head in a type position warns when the enclosing type
//!   declares a same-named nested type
//! rule: Chain head — name followed by `.`: enclosing-type members rank last,
//!   after aliases — a reachable type outranks a same-named member, and the
//!   shadowed member stays reachable by qualifying (`this.Data.W()`; the
//!   enclosing type's name for statics), which for a nested type is
//!   `Outer.Choice`. Warning row — chain head resolves to a type while a
//!   same-named accessible member exists: warn, *unless* the member's declared
//!   type is the resolved type (or an instantiation of the resolved generic).
//! cites: 2.7
//! expect: warn
//! diagnostic: chain head
//! issue: 7085
module Conf;

public class Choice
  public class Item
    public method Tag(): int return 1; end Tag;
  end Item;
end Choice;

public class Outer
  public class Choice
    public class Item
      public method Tag(): int return 2; end Tag;
    end Item;
  end Choice;
  public class Inner
    public static method Main(): int
      using var i: Choice.Item := new Choice.Item();
      Console.WriteLine(i.Tag());
      return 0;
    end Main;
  end Inner;
end Outer;
