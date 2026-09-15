//! case: s28_6_0006
//! doc: spec
//! section: 28.6
//! title: an unmarked mint-announcing method may not lend a stored frame field
//! rule: A mint-announcing-typed field is not returnable through an unmarked
//!   mint-announcing method return, that return type being
//!   container-ownership-transferring; a stored frame is instead re-exposed
//!   through a property, field, or collection slot.
//! cites: 28.3, 28.6
//! expect: reject
//! diagnostic: lend
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

  public method Items(): IEnumerable<Item>
    return _items;
  end Items;
end Basket;
