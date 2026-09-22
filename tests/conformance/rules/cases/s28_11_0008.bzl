//! case: s28_11_0008
//! doc: spec
//! section: 28.11
//! title: an element borrow used after a dominating RemoveAt at its index draws the use-after-removal warning
//! rule: The compiler must warn when a local holding such a borrow is used
//!   after a call on the same collection binding that disposes the borrowed
//!   element and dominates the use on every path — `RemoveAt(i)` where the
//!   borrow was taken as `coll[i]` with the same index expression.
//! cites: 28.11, 28.2, 28.3
//! expect: warn
//! diagnostic: removal
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
  end Ping;

  destructor
  end destructor;
end Widget;

public class Program
  public method Demo()
    using var xs := new OwningArray<Widget>();
    xs.Add(new Widget());
    var w := xs[0];
    xs.RemoveAt(0);
    w.Ping();
  end Demo;
end Program;
