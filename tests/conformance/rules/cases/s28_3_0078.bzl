//! case: s28_3_0078
//! doc: spec
//! section: 28.3
//! title: the null-forgiving dispose spelling suppresses the must-use warning
//! rule: The warning is **suppressed** ... when it is explicitly disposed or
//!   discarded — including through the null-conditional and null-forgiving
//!   spellings `x?.Dispose()` / `x!.Dispose()`, required for nullable owned locals.
//! cites: 28.3, 17.8
//! expect: quiet
module Conf;

public class Handle
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("handle released");
  end destructor;
end Handle;

public class Session
  public method Work()
    var h: Handle? := new Handle();
    h!.Dispose();
  end Work;
end Session;
