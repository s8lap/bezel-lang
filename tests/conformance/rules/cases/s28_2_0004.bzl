//! case: s28_2_0004
//! doc: spec
//! section: 28.2
//! title: a straight-line read after a discard raises the same mandatory warning
//! rule: The compiler must emit a warning when a local variable is used after a discard
//!   of the binding that dominates the use site on every path.
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
    _ := w;
    var text := w.ToString();
  end Demo;
end Program;
