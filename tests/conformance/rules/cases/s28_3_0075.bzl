//! case: s28_3_0075
//! doc: spec
//! section: 28.3
//! title: a move into an owned argument suppresses the must-use warning
//! rule: The warning is **suppressed** by exactly these conditions ... it is
//!   moved or returned (no implicit drop occurs)
//! cites: 28.3, 28.8.1
//! expect: quiet
module Conf;

public class Handle
  public constructor()
  end constructor;

  public method Touch()
    Console.WriteLine("touched");
  end Touch;

  destructor
    Console.WriteLine("handle released");
  end destructor;
end Handle;

public class Sink
  public constructor()
  end constructor;

  public method Take(owned h: Handle)
    using var kept := move h;
    kept.Touch();
  end Take;
end Sink;

public class Session
  public method Work(sink: Sink)
    var h := new Handle();
    h.Touch();
    sink.Take(move h);
  end Work;
end Session;
