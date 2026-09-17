//! case: s28_6_0002
//! doc: spec
//! section: 28.6
//! title: a freshly-produced owned value stored into a non-owned field is rejected
//! rule: A freshly-produced owned value — a `new` expression, a call returning
//!   `owned T`, a `move` expression, or an owned operator result — stored into
//!   a non-`owned` (borrow) field is an error.
//! cites: 28.5, 28.6
//! expect: reject
//! diagnostic: freshly-produced
module Conf;

public class Cell
  public constructor()
  end constructor;
end Cell;

public class Handle
  private _c: Cell;

  public constructor()
    _c := new Cell();
  end constructor;
end Handle;
