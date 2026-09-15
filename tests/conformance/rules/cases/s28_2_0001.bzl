//! case: s28_2_0001
//! doc: spec
//! section: 28.2
//! title: Dispose is idempotent at run time: a second explicit call performs no second disposal
//! rule: `Dispose()` on any class instance is idempotent: the first call
//!   performs the disposal; subsequent calls are no-ops. That no-op carve-out
//!   states the **runtime** contract of a repeated call and suppresses no
//!   diagnostic — a second `Dispose()` on the same binding is a use of it and
//!   draws §28.2's use-after-dispose warning, which this case observes only as
//!   a compile that succeeds (the corpus has no runs-and-warns tier).
//! cites: 28.2, 28.4
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
