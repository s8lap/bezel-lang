//! case: s22_3_0009
//! doc: spec
//! section: 22.3
//! title: writing move at a non-owned parameter position is rejected
//! rule: Writing [the move marker] ... on a non-`owned` parameter position is
//!   likewise an error (at a non-`owned` position the diagnostic names the
//!   borrow default — an owned expression may not feed a borrow slot).
//! cites: 22.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  public constructor()
  end constructor;

  public method Inspect(item: Widget)
  end Inspect;
end Registry;

public class Program
  public static method Main()
    using var r := new Registry();
    var w := new Widget();
    r.Inspect(move w);
  end Main;
end Program;
