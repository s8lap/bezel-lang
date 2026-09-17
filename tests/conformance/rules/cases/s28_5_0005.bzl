//! case: s28_5_0005
//! doc: spec
//! section: 28.5
//! title: owned on a class-typed field is admitted (function-typed field twin)
//! rule: Only fields whose type is a class type or an interface type may be
//!   `owned`; a class type is admitted where a function type is not.
//! cites: 28.1, 28.5
//! expect: accept
module Conf;

public class Worker
  public constructor()
  end constructor;
end Worker;

public class Registry
  private owned _cb: Worker;

  public constructor()
    _cb := new Worker();
  end constructor;
end Registry;
