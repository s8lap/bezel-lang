//! case: s28_6_0005
//! doc: spec
//! section: 28.6
//! title: a factory method returning owned is the remedy for a minting getter
//! rule: A property or indexer getter may not return a freshly-produced owned
//!   value; expose the factory as a method returning `owned T` instead, which
//!   can hand the caller ownership.
//! cites: 28.6
//! expect: accept
module Conf;

public class Cell
  public constructor()
  end constructor;
end Cell;

public class Handle
  public method MakeItem(): owned Cell => new Cell();
end Handle;
