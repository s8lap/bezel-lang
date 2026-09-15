//! case: s28_5_0010
//! doc: spec
//! section: 28.5
//! title: an owned field on a record is rejected
//! rule: Records may not have `owned` fields (records support only copy
//!   semantics, not move semantics).
//! cites: 9.7, 28.5
//! expect: reject
//! diagnostic: owned fields
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Holder
  private owned _c: Widget;

  public constructor(owned c: Widget)
    _c := c;
  end constructor;
end Holder;
