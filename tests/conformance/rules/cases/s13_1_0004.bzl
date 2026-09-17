//! case: s13_1_0004
//! doc: spec
//! section: 13.1
//! title: an internal member is visible from another class in the same module
//! rule: `internal` — Visible to code in the same module. Unlike `private`, an
//!   internal member is reachable from any class compiled as part of the same
//!   module, not only the declaring class.
//! cites: 13.1
//! expect: accept
module Conf;

public class Box
  internal _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;
end Box;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
