//! case: s27_5_0003
//! doc: spec
//! section: 27.5
//! title: a closure may not assign to a captured owned binding
//! rule: For a captured owned binding the sharing is read-only in the
//!   ownership sense — the lambda may not assign to it, transfer from it, or
//!   dispose it (§28.3).
//! cites: 27.5, 28.3
//! expect: reject
//! diagnostic: reassign
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    var replace: () -> () := lambda() => do w := new Widget(); end;
    replace();
    w.Dispose();
  end Demo;
end Program;
