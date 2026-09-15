//! case: s14_2_0004
//! doc: spec
//! section: 14.2
//! title: a class fails where T: value
//! rule: `where T: value` admits only non-nullable value types; a reference
//!   type is excluded (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: value
//! issue: 4367
module Conf;

public class Box<T> where T: value
  public constructor()
  end constructor;
end Box;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _b: Box<Widget>?;

  public constructor()
  end constructor;
end Holder;
