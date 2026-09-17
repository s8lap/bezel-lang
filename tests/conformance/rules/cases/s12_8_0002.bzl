//! case: s12_8_0002
//! doc: spec
//! section: 12.8
//! title: a chain argument may not read a field of this
//! rule: it may not read a field or auto-property of the object under
//!   construction -- nothing of this is assigned yet.
//! cites: 12.8, 22.7
//! expect: reject
//! diagnostic: chain
module Conf;

public class Base
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;
end Base;

public class Derived: Base
  private _extra: int := 5;

  public constructor(n: int)
    base(n + _extra);
  end constructor;
end Derived;
