//! case: s28_2_0001
//! doc: spec
//! section: 28.2
//! title: Dispose is idempotent: a second explicit call is a no-op
//! rule: `Dispose()` on any class instance is idempotent: the first call performs the
//!   disposal; subsequent calls are no-ops.
//! cites: 28.4
//! expect: run
//! output: disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    w.Dispose();
    w.Dispose();
  end Main;
end Program;
