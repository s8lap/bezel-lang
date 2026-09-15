//! case: s14_2_0006
//! doc: spec
//! section: 14.2
//! title: a nullable class type fails where T: notnull
//! rule: `where T: notnull` excludes a nullable type, value or reference — a
//!   nullable type is excluded (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: notnull
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
  private _b: Box<Widget?>?;

  public constructor()
  end constructor;
end Holder;
