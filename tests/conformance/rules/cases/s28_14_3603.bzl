//! case: s28_14_3603
//! doc: spec
//! section: 28.14
//! title: a scoped construction into an owned field of this over a non-this source is rejected
//! rule: where every captured source is `this` itself or a value whose borrow provably originates from
//!   `this` — the only things the stored callback can dangle on are then the very object whose teardown
//!   bounds the storage
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
//! issue: 8397
//! notes: Rejecting twin of s28_14_3601: the one edit is the scoped argument, a plain constructor
//!   parameter `p` instead of `this`.
module Conf;

public class Item
  public constructor() end constructor;
end Item;

public class Child
  private _it: Item;
  public constructor(scoped it: Item)
    _it := it;
  end constructor;
end Child;

public class Owner
  private owned _child: Child?;
  public constructor(p: Item)
    _child := new Child(scoped p);
  end constructor;
end Owner;
