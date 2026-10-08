//! case: s28_3_0106
//! doc: spec
//! section: 28.3
//! title: a scoped parameter's value may not be stored into an out location
//! rule: A borrow sourced at a `scoped` parameter is not a plain-parameter source
//!   and is rejected there: its own value, which §28.14 admits only into the
//!   bounding object's fields, a borrow reached through it, and the result of
//!   invoking it.
//! cites: 28.3, 28.14
//! expect: reject
//! diagnostic: scoped
//! issue: 5625
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Binder
  public constructor()
  end constructor;

  public method Fill(scoped s: Item, out o: Item)
    o := s;
  end Fill;
end Binder;
