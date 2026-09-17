//! case: s12_8_0001
//! doc: spec
//! section: 12.8
//! title: a base(...) chain argument may read the constructor's own parameters and constants
//! rule: The only reads a base(...)/this(...) argument may make are of the
//!   constructor's own parameters, constants, and statics.
//! cites: 12.8
//! expect: accept
module Conf;

public class Base
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;
end Base;

public class Derived: Base
  public const Factor: int := 2;

  public constructor(n: int)
    base(n * Derived.Factor);
  end constructor;
end Derived;
