//! case: s28_14_0017
//! doc: spec
//! section: 28.14
//! title: scoped on a static method with an owned return resolves the returned object as the bound
//! rule: A scoped parameter on a static method is admitted when the
//!   method's return is owned, so that a call site can resolve bounding
//!   form 2 or 3.
//! cites: 28.14
//! expect: run
//! stdout: fired
module Conf;

public class Registry
  private scoped _cb: () -> ();

  private constructor()
  end constructor;

  public static method Create(scoped cb: () -> ()): owned Registry
    var r := new Registry();
    r._cb := cb;
    return r;
  end Create;

  public method Fire()
    _cb();
  end Fire;
end Registry;

public class Program
  public static method Main()
    using var reg := Registry.Create(lambda() => Console.WriteLine("fired"));
    reg.Fire();
  end Main;
end Program;
