//! case: s28_2_0004
//! doc: spec
//! section: 28.2
//! title: a straight-line read after a discard of the binding is the same compile error
//! rule: "A use of a local variable ... after an explicit `Dispose()` or direct
//!   `DisposeAsync()` call — or a discard of the binding (§17.13) — on the same
//!   variable that dominates the use site on every path is a **compile error**"
//!   (§28.2, v0.357).
//! cites: 28.2, 17.13
//! expect: reject
//! diagnostic: use-after-dispose
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
