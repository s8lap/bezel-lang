//! case: s28_6_0020
//! doc: spec
//! section: 28.6
//! title: disposing a captured owned binding inside a closure is rejected
//! rule: The borrow receivers for `Dispose()` include a captured owned
//!   binding inside a closure.
//! cites: 27.5, 28.3, 28.6
//! expect: reject
//! diagnostic: closure
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    var cleanup: () -> () := lambda() => w.Dispose();
    cleanup();
  end Demo;
end Program;
