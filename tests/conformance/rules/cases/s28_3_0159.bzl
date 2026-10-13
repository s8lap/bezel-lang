//! case: s28_3_0159
//! doc: spec
//! section: 28.3
//! title: a plain parameter may be assigned a borrow of a top-level using var
//! rule: A parameter's scope is the body's outermost block for this comparison: it
//!   does not strictly enclose a local declared at the body's top level, so a
//!   whole-value assignment `p := it` to a plain parameter ... are admitted for
//!   such a source and rejected for a source declared in a nested block
//! cites: 28.3
//! expect: run
//! stdout: 1
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Use(p: Item): int
    using var it := new Item();
    p := it;
    return p.N();
  end Use;

  public static method Main()
    using var a := new Item();
    Console.WriteLine(Use(a).ToString());
  end Main;
end Program;
