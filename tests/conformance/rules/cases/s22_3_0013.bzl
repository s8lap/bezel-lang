//! case: s22_3_0013
//! doc: spec
//! section: 22.3
//! title: a ref argument may not name a location of owned state
//! rule: A `ref` or `out` argument must be an assignable storage location,
//!   subject to two recorded v1 bounds (liftable): no location of owned
//!   state — an owned local or `owned` parameter, an `owned` field, an
//!   owning-collection or owning-inline-array slot.
//! cites: 22.3
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Replace(ref w: Widget)
  end Replace;

  public static method Main()
    var w := new Widget();
    Replace(ref w);
  end Main;
end Program;
