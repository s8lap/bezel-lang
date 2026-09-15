//! case: s28_3_0033
//! doc: spec
//! section: 28.3
//! title: reading a variable that is owned on one path and moved-from on another is rejected
//! rule: owned meets moved-from is maybe-consumed at the join; a later use is a compile
//!   error.
//! expect: reject
//! diagnostic: consumed
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
  public method Demo(flag: bool, sink: Sink)
    var w := new Widget();
    if flag then
      sink.Adopt(move w);
    end;
    w.Dispose();
  end Demo;
end Program;
