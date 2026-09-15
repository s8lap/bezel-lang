//! case: s28_3_0004
//! doc: spec
//! section: 28.3
//! title: an owned argument fed without move is rejected
//! rule: Omitting `move` is a compile error when a named owned binding feeds an `owned`
//!   argument: write `M(move x)`.
//! expect: reject
//! diagnostic: consumes
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Sink
  public method Adopt(owned w: Widget)
    w.Dispose();
  end Adopt;
end Sink;

public class Program
  public method Demo()
    var s := new Sink();
    var w := new Widget();
    s.Adopt(w);
  end Demo;
end Program;
