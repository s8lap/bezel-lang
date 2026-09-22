//! case: s28_11_0009
//! doc: spec
//! section: 28.11
//! title: using an element borrow after a dominating Clear on the same collection draws the warning
//! rule: The compiler must warn when a local holding such a borrow is used
//!   after a call on the same collection binding that disposes the borrowed
//!   element and dominates the use on every path — `Clear()`, which disposes
//!   every element.
//! cites: 28.11, 28.2
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
    xs.Clear();
    w.Ping();
  end Demo;
end Program;
