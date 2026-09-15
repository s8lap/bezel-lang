//! case: s22_7_0001
//! doc: spec
//! section: 22.7
//! title: a constructor may chain to another constructor of the same class with this
//! rule: Multiple constructors are permitted; chaining uses `this(...)` or
//!   `base(...)` in the chaining position before the body.
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
