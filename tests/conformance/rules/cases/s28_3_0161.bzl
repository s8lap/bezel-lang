//! case: s28_3_0161
//! doc: spec
//! section: 28.3
//! title: a parameter assigned a local borrow carries that source to a later field store
//! rule: A parameter's scope is the body's outermost block for this comparison: it
//!   does not strictly enclose a local declared at the body's top level, so a
//!   whole-value assignment `p := it` to a plain parameter ... are admitted for
//!   such a source and rejected for a source declared in a nested block; the parameter's later escapes — a return, a field store, a capture —
//!   are read off its source set, which carries the assigned source
//! cites: 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Keeper
  private _k: Item;

  public constructor(k: Item)
    _k := k;
  end constructor;

  public method Swap(p: Item)
    using var it := new Item();
    p := it;
    _k := p;
  end Swap;
end Keeper;
