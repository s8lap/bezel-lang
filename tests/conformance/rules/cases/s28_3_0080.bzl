//! case: s28_3_0080
//! doc: spec
//! section: 28.3
//! title: the using var move adoption suppresses the owned-parameter must-use warning
//! rule: The adoption idiom `using var r := move reader;` suppresses the must-use
//!   warning on an `owned` parameter exactly as `using var` suppresses the local form.
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
  public method Consume(owned h: Handle)
    using var kept := move h;
    kept.Touch();
  end Consume;
end Session;
