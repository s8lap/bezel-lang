//! case: s28_3_0073
//! doc: spec
//! section: 28.3
//! title: an explicit Dispose suppresses the must-use warning on an owned local
//! rule: The warning is **suppressed** by exactly these conditions ... it is
//!   explicitly disposed or discarded
//! cites: 28.3, 28.2
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

public class Session
  public method Work()
    var h := new Handle();
    h.Touch();
    h.Dispose();
  end Work;
end Session;
