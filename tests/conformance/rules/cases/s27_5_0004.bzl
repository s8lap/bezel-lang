//! case: s27_5_0004
//! doc: spec
//! section: 27.5
//! title: a closure may not move a captured owned binding
//! rule: `move` is an error inside a closure over a captured owned binding —
//!   captured owned bindings are uniformly in borrow state, so a closure
//!   cannot consume, reassign, or dispose one (§28.3).
//! cites: 27.5, 28.3, 28.8.1
//! expect: reject
//! diagnostic: closure
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

  public method Stash(owned w: Widget)
    var take: () -> () := lambda() => _items.Add(move w);
    take();
  end Stash;
end Pool;
