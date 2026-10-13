//! case: s28_11_0029
//! doc: spec
//! section: 28.11
//! title: a local collection holding a using var's borrow may not be returned
//! rule: a borrow of a scope-bound binding ... stored into a collection that
//!   provably outlives that scope — the collection is a field, is reachable from
//!   one, is held by a parameter that is not `owned` or reached through one
//!   (caller storage, which outlives every scope of the body), escapes the
//!   method, or is a local of strictly wider scope — is rejected at the store
//!   site
//! cites: 28.11, 9.7
//! expect: reject
//! diagnostic: outlive
//! issue: 5735
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Maker
  public static method Make(): owned Array<Item>
    var coll := new Array<Item>();
    using var item := new Item();
    coll.Add(item);
    return coll;
  end Make;
end Maker;
