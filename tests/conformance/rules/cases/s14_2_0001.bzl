//! case: s14_2_0001
//! doc: spec
//! section: 14.2
//! title: a class type satisfies where T: class
//! rule: `where T: class` accepts non-nullable reference types only — classes,
//!   interfaces, string, function types (§14.2).
//! cites: 14.2
//! expect: accept
//! issue: 4367
module Conf;

public class Box<T> where T: class
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
