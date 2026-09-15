//! case: s28_6_0004
//! doc: spec
//! section: 28.6
//! title: a property getter returning a fresh owned value is rejected
//! rule: A property or indexer getter may not return a freshly-produced owned
//!   value. A getter is a borrow-producing position, and a fresh owned value
//!   returned through it would escape with no owner and leak.
//! cites: 28.6
//! expect: reject
//! diagnostic: getter
module Conf;

public class Cell
  public constructor()
  end constructor;
end Cell;

public class Handle
  public property Item: Cell
    get => new Cell();
  end Item;
end Handle;
