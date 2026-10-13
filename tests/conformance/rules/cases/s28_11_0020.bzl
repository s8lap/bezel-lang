//! case: s28_11_0020
//! doc: spec
//! section: 28.11
//! title: AddRange from a plain parameter into a field-held collection is admitted
//! rule: So `_all.AddRange(items)` from a plain parameter and `return _items.ToArray()`
//!   over a field are admitted under the residual, and `_all.AddRange(local)` and
//!   `return local.ToArray()` over a scope-bound local are rejected, as
//!   `_all.Add(local[0])` is
//! cites: 28.11
//! expect: accept
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

  public method Merge(items: Array<Item>)
    _all.AddRange(items);
  end Merge;
end Index;
