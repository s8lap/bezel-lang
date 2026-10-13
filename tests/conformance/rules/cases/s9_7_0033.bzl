//! case: s9_7_0033
//! doc: spec
//! section: 9.7
//! title: a whole-value assignment resets a record local's set before a later member store
//! rule: a whole-value assignment gives the location the assigned value's set, and a
//!   member store ... adds the sources of the borrows `e` carries to that set
//! cites: 9.7, 28.3
//! expect: accept
//! issue: 5734
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public record Box
  public property Held: Item?;

  public constructor()
    Held := null;
  end constructor;
end Box;

public class Keeper
  private owned _item: Item;

  public constructor()
    _item := new Item();
  end constructor;

  public method Make(): Box
    using var it := new Item();
    var p := new Box();
    p.Held := it;
    p := new Box();
    p.Held := _item;
    return p;
  end Make;
end Keeper;
