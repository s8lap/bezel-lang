//! case: s28_2_0003
//! doc: spec
//! section: 28.2
//! title: a straight-line read after an explicit Dispose raises the use-after-dispose warning
//! rule: The compiler must emit a warning when a local variable is used after an
//!   explicit `Dispose()` call on the same variable that dominates the use site on
//!   every path.
//! expect: warn
//! diagnostic: used
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    w.Dispose();
    var text := w.ToString();
  end Demo;
end Program;
