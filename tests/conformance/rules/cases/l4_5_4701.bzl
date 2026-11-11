//! case: l4_5_4701
//! doc: stdlib
//! section: 4.5
//! title: a new expression added in a constructor to an owned OwningArray field is admitted
//! rule: public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>,
//!   IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
//! cites: 15.4, 28.5
//! expect: quiet
module Conf;

public class Item
  public V: int;

  public constructor(v: int)
    this.V := v;
  end constructor;
end Item;

public class Holder
  private owned _a: OwningArray<Item>;

  public constructor()
    this._a := new OwningArray<Item>();
    this._a.Add(new Item(3));
  end constructor;
end Holder;
