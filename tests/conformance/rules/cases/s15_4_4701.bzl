//! case: s15_4_4701
//! doc: spec
//! section: 15.4
//! title: a new expression added in a constructor to an owned Array field is a fresh-store error
//! rule: in a single-element store position of a borrowing-family collection is a compile error at the
//!   store site.
//! cites: 28.3.11, 28.6, 28.11
//! expect: reject
//! diagnostic: fresh-store
module Conf;

public class Item
  public V: int;

  public constructor(v: int)
    this.V := v;
  end constructor;
end Item;

public class Holder
  private owned _a: Array<Item>;

  public constructor()
    this._a := new Array<Item>();
    this._a.Add(new Item(3));
  end constructor;
end Holder;
