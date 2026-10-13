//! case: s28_3_0142
//! doc: spec
//! section: 28.3
//! title: invoking a closure whose captured binding is later assigned a using var cannot be returned
//! rule: A captured binding — a local or parameter, in any state, that a lambda in
//!   the body captures — holds, for this count and wherever else its source set
//!   is read ... every value it is ever assigned ... and a `return f();` or `return b;` after `b := it;` is rejected too
//! cites: 28.3, 27.5
//! expect: reject
//! diagnostic: outlive
//! issue: 5718
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Holder
  private owned _item: Item;

  public constructor()
    _item := new Item();
  end constructor;

  public method Get(): Item
    var b := _item;
    var f := lambda() => b;
    using var it := new Item();
    b := it;
    return f();
  end Get;
end Holder;
