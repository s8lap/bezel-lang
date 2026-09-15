//! case: s28_3_0007
//! doc: spec
//! section: 28.3
//! title: assigning a marked transfer into an owned-state local is admitted
//! rule: An owned-state LHS accepts a named owned binding under the explicit transfer
//!   marker: `x := move y;`, disposing the displaced value.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var best := new Widget();
    var next := new Widget();
    best := move next;
    best.Dispose();
  end Demo;
end Program;
