//! case: s28_6_0003
//! doc: spec
//! section: 28.6
//! title: a freshly-produced owned value stored into an owned field is admitted
//! rule: A freshly-produced owned value stored into a non-`owned` field is an
//!   error; declaring the field `owned` gives the containing object a place to
//!   hold and dispose it.
//! cites: 28.5, 28.6
//! expect: accept
module Conf;

public class Cell
  public constructor()
  end constructor;
end Cell;

public class Handle
  private owned _c: Cell;

  public constructor()
    _c := new Cell();
  end constructor;
end Handle;
