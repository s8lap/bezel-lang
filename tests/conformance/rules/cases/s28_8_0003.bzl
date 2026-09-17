//! case: s28_8_0003
//! doc: spec
//! section: 28.8
//! title: the caller may not use an argument it transferred to an owned parameter
//! rule: When a parameter is `owned`, the caller relinquishes ownership and may
//!   not use the argument after the call (§28.8, K3.3).
//! cites: 28.8, 28.3
//! expect: reject
//! diagnostic: after the call
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("ping");
  end Ping;
end Widget;

public class Sink
  public static method Take(owned w: Widget)
    w.Dispose();
  end Take;
end Sink;

public class Program
  public static method Run()
    var w := new Widget();
    Sink.Take(move w);
    w.Ping();
  end Run;
end Program;
