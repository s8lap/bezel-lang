//! case: s28_6_0001
//! doc: spec
//! section: 28.6
//! title: a bare read of an owned field returns as a receiver-sourced borrow
//! rule: A bare read of an owned field lends and may be returned through a
//!   non-`owned` return type as a borrow of the field — its source is the
//!   receiver.
//! cites: 28.3, 28.5, 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Pool
  private owned _items: OwningArray<Widget>;

  public constructor()
    _items := new OwningArray<Widget>();
  end constructor;

  public method Items(): OwningArray<Widget>
    return _items;
  end Items;
end Pool;
