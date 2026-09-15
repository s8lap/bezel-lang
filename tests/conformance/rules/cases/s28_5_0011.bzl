//! case: s28_5_0011
//! doc: spec
//! section: 28.5
//! title: a plain (non-owned) class-typed field on a record is admitted
//! rule: Records may not have `owned` fields; a plain reference-typed field
//!   (a borrow) is ordinary record storage.
//! cites: 9.7, 28.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Holder
  private _c: Widget;

  public constructor(c: Widget)
    _c := c;
  end constructor;
end Holder;
