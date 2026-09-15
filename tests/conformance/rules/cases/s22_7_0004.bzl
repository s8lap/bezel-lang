//! case: s22_7_0004
//! doc: spec
//! section: 22.7
//! title: a chain argument may read the delegating constructor's own parameters
//! rule: A chain argument may read only the constructor's own parameters,
//!   constants and statics — never a field of `this`.
//! cites: 22.7, 12.8
//! expect: accept
module Conf;

public class Point
  private _x: int;
  private _y: int;

  public constructor(x: int, y: int)
    _x := x;
    _y := y;
  end constructor;

  public constructor(x: int)
    this(x, 0);
  end constructor;
end Point;
