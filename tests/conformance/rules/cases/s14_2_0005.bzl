//! case: s14_2_0005
//! doc: spec
//! section: 14.2
//! title: a non-nullable class satisfies where T: notnull
//! rule: `where T: notnull` accepts non-nullable types only, value or
//!   reference (§14.2).
//! cites: 14.2
//! expect: accept
module Conf;

public class Box<T> where T: notnull
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
