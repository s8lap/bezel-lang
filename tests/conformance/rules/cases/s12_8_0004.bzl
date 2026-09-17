//! case: s12_8_0004
//! doc: spec
//! section: 12.8
//! title: the implicit base() insertion requires a parameterless base constructor
//! rule: If neither base(...) nor this(...) is written, the compiler
//!   inserts an implicit base() call (which requires the base class to have
//!   a parameterless constructor; otherwise the chain must be explicit).
//! cites: 12.8
//! expect: reject
//! diagnostic: parameterless
module Conf;

public class Base
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;
end Base;

public class Derived: Base
  public constructor()
  end constructor;
end Derived;
