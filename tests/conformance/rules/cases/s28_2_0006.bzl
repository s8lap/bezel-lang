//! case: s28_2_0006
//! doc: spec
//! section: 28.2
//! title: every class implicitly implements IDisposable via a generated Dispose
//! rule: Every class implicitly implements `IDisposable` via a compiler-generated
//!   `Dispose()`; user code declares no `Dispose` member, and the generated one is
//!   callable on an instance the caller owns.
//! cites: 28.6
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
