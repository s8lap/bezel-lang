//! case: s28_3_0107
//! doc: spec
//! section: 28.3
//! title: a plain parameter's value may be stored into an out location
//! rule: A `ref` or plain `out` argument location whose type, after substitution,
//!   can carry a borrow ... is caller storage: the callee may store into it a
//!   borrow sourced at its receiver, a plain parameter, or static storage
//! cites: 28.3, 22.3
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Binder
  public constructor()
  end constructor;

  public method Fill(s: Item, out o: Item)
    o := s;
  end Fill;

  public method Use(p: Item): int
    Fill(p, out var o);
    return o.Size();
  end Use;
end Binder;
