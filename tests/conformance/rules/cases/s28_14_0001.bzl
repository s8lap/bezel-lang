//! case: s28_14_0001
//! doc: spec
//! section: 28.14
//! title: a scoped callback parameter may be stored in a scoped field of the constructed bounding object
//! rule: The callee may store the callback, but only in scoped fields of
//!   the bounding object itself; a constructor call resolves the bounding
//!   object to the constructed object.
//! cites: 28.14
//! expect: run
//! stdout: fired
module Conf;

public class Registry
  private scoped _cb: () -> ();

  public constructor(scoped cb: () -> ())
    _cb := cb;
  end constructor;

  public method Fire()
    _cb();
  end Fire;
end Registry;

public class Program
  public static method Main()
    var reg := new Registry(lambda() => Console.WriteLine("fired"));
    reg.Fire();
  end Main;
end Program;
