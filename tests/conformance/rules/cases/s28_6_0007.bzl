//! case: s28_6_0007
//! doc: spec
//! section: 28.6
//! title: a stored mint-announcing view is re-exposed through a property
//! rule: A mint-announcing-typed field is not returnable through an unmarked
//!   mint-announcing method return; a stored frame is instead re-exposed
//!   through a property, field, or collection slot.
//! cites: 28.3, 28.6
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Basket
  private _items: IEnumerable<Item>;

  public constructor(source: IEnumerable<Item>)
    _items := source;
  end constructor;

  public property Items: IEnumerable<Item> => _items;
end Basket;
