//! case: s22_7_0003
//! doc: spec
//! section: 22.7
//! title: a this-chain argument may not read a field of the not-yet-constructed instance
//! rule: In the chaining position (`base(...)`/`this(...)` arguments) no
//!   field of the declaring type is yet assigned, so any observation of
//!   `this` there is a compile error.
//! cites: 22.7
//! expect: reject
//! diagnostic: chaining position
module Conf;

public class Point
  private _x: int := 1;
  private _y: int;

  public constructor(x: int, y: int)
    _x := x;
    _y := y;
  end constructor;

  public constructor()
    this(_x, 0);
  end constructor;
end Point;
