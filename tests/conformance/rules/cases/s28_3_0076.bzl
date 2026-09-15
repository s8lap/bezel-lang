//! case: s28_3_0076
//! doc: spec
//! section: 28.3
//! title: returning an owned local under an owned return suppresses the must-use warning
//! rule: The warning is **suppressed** ... when it is moved or returned (no
//!   implicit drop occurs); a `return` operand under an `owned` return is a
//!   marker-free position.
//! cites: 28.3, 28.8
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
  public method Acquire(): owned Handle
    var h := new Handle();
    h.Touch();
    return h;
  end Acquire;
end Session;
