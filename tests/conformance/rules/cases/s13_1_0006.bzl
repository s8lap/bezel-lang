//! case: s13_1_0006
//! doc: spec
//! section: 13.1
//! title: a protected member is not reachable from an unrelated class in the same module
//! rule: `protected` — Visible to the declaring class and all classes derived
//!   from it. §13.1: an unrelated class in the same module sees `internal`
//!   but not `protected` — module membership alone does not confer access;
//!   only the inheritance relationship does.
//! cites: 13.1
//! expect: reject
//! diagnostic: protected
module Conf;

public class Base
  protected _secret: int;

  public constructor(secret: int)
    _secret := secret;
  end constructor;
end Base;

public class Unrelated
  public constructor()
  end constructor;

  public method Peek(b: Base): int => b._secret;
end Unrelated;
