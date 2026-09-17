//! case: s28_3_0072
//! doc: spec
//! section: 28.3
//! title: using var suppresses the must-use warning on an owned local
//! rule: The warning is **suppressed** when the local is declared `using var`
//!   (the scope-bound intent is then explicit).
//! cites: 28.3, 28.10
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
    using var h := new Handle();
    h.Touch();
  end Work;
end Session;
