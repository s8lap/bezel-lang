//! case: s28_11_0033
//! doc: spec
//! section: 28.11
//! title: a top-level local's borrow stored into an owned parameter's collection is admitted
//! rule: A collection an `owned` parameter holds is the body's own ... and is
//!   read as a local of the body's outermost block ... a store of a
//!   top-level local's borrow into it is admitted under the residual below
//! cites: 28.11, 28.3
//! expect: accept
//! issue: 5922
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Sink
  public static method Fill(owned xs: Array<Item>): int
    using var it := new Item();
    xs.Add(it);
    return xs.Count;
  end Fill;
end Sink;
