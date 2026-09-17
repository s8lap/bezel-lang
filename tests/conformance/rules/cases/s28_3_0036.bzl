//! case: s28_3_0036
//! doc: spec
//! section: 28.3
//! title: an explicit Dispose while a borrow is live is not frozen
//! rule: Site-announced kills are unfrozen: an explicit `Dispose()` is permitted while
//!   a live local borrow of the value exists — a borrow is live when some path from
//!   the consume site reaches a later use of it.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    var b := w;
    w.Dispose();
    var alias := b;
  end Demo;
end Program;
