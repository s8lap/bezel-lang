//! case: s28_2_0003
//! doc: spec
//! section: 28.2
//! title: a straight-line read after an explicit Dispose is a compile error
//! rule: "A use of a local variable — a read, a borrow taken from it, a member access,
//!   an argument position, a consume — after an explicit `Dispose()` or direct
//!   `DisposeAsync()` call ... on the same variable that dominates the use site on
//!   every path is a **compile error**" (§28.2, v0.357).
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
    w.Dispose();
    var text := w.ToString();
  end Demo;
end Program;
