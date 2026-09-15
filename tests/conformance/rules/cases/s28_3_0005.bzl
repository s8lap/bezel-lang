//! case: s28_3_0005
//! doc: spec
//! section: 28.3
//! title: an owned argument fed with move is admitted
//! rule: `move x` is the required spelling for a named owned binding feeding an `owned`
//!   argument.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Sink
  public constructor()
  end constructor;

  public method Adopt(owned w: Widget)
    w.Dispose();
  end Adopt;
end Sink;

public class Program
  public method Demo()
    var s := new Sink();
    var w := new Widget();
    s.Adopt(move w);
  end Demo;
end Program;
