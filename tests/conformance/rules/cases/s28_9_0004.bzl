//! case: s28_9_0004
//! doc: spec
//! section: 28.9
//! title: a closure may not dispose an owned local it captures
//! rule: Capturing an owned class-typed local does not transfer ownership
//!   into the closure — the closure may not assign to, transfer from, or
//!   dispose the captured binding.
//! cites: 28.9, 28.3
//! expect: reject
//! diagnostic: dispose
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Run()
    using var w := new Widget();
    var closer := lambda() => w.Dispose();
    closer();
  end Run;
end Program;
