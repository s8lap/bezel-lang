//! case: s28_3_0017
//! doc: spec
//! section: 28.3
//! title: a store into an owned field carries no marker
//! rule: The declared `owned` field already announces the transfer; the store is
//!   marker-free.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Pool
  private owned _w: Widget;

  public constructor(owned w: Widget)
    _w := w;
  end constructor;
end Pool;
