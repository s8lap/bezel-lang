//! case: s7_9_0001
//! doc: spec
//! section: 7.9
//! title: a non-nullable field with no initializer and no constructor assignment is rejected
//! rule: Every non-nullable field must be either assigned a value in every
//!   constructor before the constructor returns, or given a field
//!   initializer at its declaration.
//! expect: reject
//! diagnostic: every constructor
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
  end constructor;
end Holder;
