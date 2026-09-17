//! case: s7_9_0002
//! doc: spec
//! section: 7.9
//! title: a non-nullable field assigned in the constructor is admitted
//! rule: Every non-nullable field must be assigned a value in every
//!   constructor before the constructor returns, or given a field
//!   initializer at its declaration.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;
end Holder;
