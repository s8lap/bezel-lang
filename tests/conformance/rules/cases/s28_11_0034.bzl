//! case: s28_11_0034
//! doc: spec
//! section: 28.11
//! title: a nested-block binding's borrow stored into an owned parameter's collection is rejected
//! rule: A collection an `owned` parameter holds is the body's own ... and is
//!   read as a local of the body's outermost block ... a store of a borrow
//!   of a binding declared in a nested block is rejected
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Sink
  public static method Fill(owned xs: Array<Item>): int
    do
      using var it := new Item();
      xs.Add(it);
    end;
    return xs.Count;
  end Fill;
end Sink;
