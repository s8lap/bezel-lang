//! case: s28_3_0016
//! doc: spec
//! section: 28.3
//! title: move on a store into an owned field is rejected as redundant
//! rule: The transfer marker is rejected as redundant on a store into an `owned` field:
//!   `sink._f := x;`, never `sink._f := move x;`.
//! expect: reject
//! diagnostic: field
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Pool
  private owned _w: Widget;

  public constructor(owned w: Widget)
    _w := move w;
  end constructor;
end Pool;
