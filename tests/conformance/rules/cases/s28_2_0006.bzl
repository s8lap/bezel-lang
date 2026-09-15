//! case: s28_2_0006
//! doc: spec
//! section: 28.2
//! title: an explicit Dispose with no later use raises no warning
//! rule: The mandatory use-after-dispose warning fires only when a later use exists; an
//!   explicit `Dispose()` call with no subsequent read is clean.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    w.Dispose();
  end Demo;
end Program;
