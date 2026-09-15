//! case: s28_3_0074
//! doc: spec
//! section: 28.3
//! title: a discard suppresses the must-use warning on an owned local
//! rule: The warning is **suppressed** ... when it is explicitly disposed or
//!   discarded — `_ := …` (§17.13).
//! cites: 28.3, 17.13
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
    _ := h;
  end Work;
end Session;
