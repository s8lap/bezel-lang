//! case: s28_3_0094
//! doc: spec
//! section: 28.3
//! title: outside the trust-admission report mode, an admitted collection-held borrow draws no diagnostic
//! rule: The trust-admission report is informative: it changes no verdict and
//!   no generated code, and outside the mode it is not emitted.
//! cites: 28.3, 28.11
//! expect: quiet
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Index
  private owned _items: Array<Item>;

  public constructor()
    _items := new Array<Item>();
  end constructor;

  public method Register(item: Item)
    _items.Add(item);
  end Register;
end Index;
