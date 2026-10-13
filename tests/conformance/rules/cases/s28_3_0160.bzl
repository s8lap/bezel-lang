//! case: s28_3_0160
//! doc: spec
//! section: 28.3
//! title: a plain parameter may not be assigned a borrow of a nested-block using var
//! rule: A parameter's scope is the body's outermost block for this comparison: it
//!   does not strictly enclose a local declared at the body's top level, so a
//!   whole-value assignment `p := it` to a plain parameter ... are admitted for
//!   such a source and rejected for a source declared in a nested block
//! cites: 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Use(p: Item): int
    do
      using var it := new Item();
      p := it;
    end;
    return p.N();
  end Use;
end Program;
