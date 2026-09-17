//! case: s28_14_0004
//! doc: spec
//! section: 28.14
//! title: a scoped callback may be forwarded to another scoped parameter with the same bounding object
//! rule: The callee may forward the callback to another scoped parameter
//!   whose bounding object is the same object.
//! cites: 28.14
//! expect: run
//! stdout: fired
module Conf;

public class Registry
  private scoped _cb: () -> ();

  public constructor(scoped cb: () -> ())
    Store(cb);
  end constructor;

  private method Store(scoped cb: () -> ())
    _cb := cb;
  end Store;

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
