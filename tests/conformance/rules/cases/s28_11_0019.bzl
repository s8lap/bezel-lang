//! case: s28_11_0019
//! doc: spec
//! section: 28.11
//! title: AddRange from a local collection of a using var into a field-held collection is rejected
//! rule: So `_all.AddRange(items)` from a plain parameter and `return _items.ToArray()`
//!   over a field are admitted under the residual, and `_all.AddRange(local)` and
//!   `return local.ToArray()` over a scope-bound local are rejected, as
//!   `_all.Add(local[0])` is
//! cites: 28.11
//! expect: reject
//! diagnostic: outlive
//! issue: 5721
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Index
  private owned _all: Array<Item>;

  public constructor()
    _all := new Array<Item>();
  end constructor;

  public method Merge()
    using var it := new Item();
    var xs: Array<Item> := [it];
    _all.AddRange(xs);
  end Merge;
end Index;
