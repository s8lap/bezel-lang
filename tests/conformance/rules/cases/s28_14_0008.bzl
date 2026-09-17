//! case: s28_14_0008
//! doc: spec
//! section: 28.14
//! title: a call-bounded callee may not forward its callback into a scoped parameter
//! rule: The callee cannot see its argument's capture set, so it cannot
//!   vouch for the §28.14 admission — a callee that needs to register a
//!   callback it received must itself declare the parameter scoped.
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
module Conf;

public class Registry
  private scoped _cb: () -> ();

  public constructor(scoped cb: () -> ())
    _cb := cb;
  end constructor;
end Registry;

public class Runner
  public method Wire(cb: () -> ())
    var reg := new Registry(cb);
  end Wire;
end Runner;
