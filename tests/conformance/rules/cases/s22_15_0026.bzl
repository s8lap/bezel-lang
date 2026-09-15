//! case: s22_15_0026
//! doc: spec
//! section: 22.15
//! title: a pure constructor's birth-store rule admits assignment to this's fields
//! rule: A constructor may be declared `pure`. Its body is checked under
//!   the pure-body rules with one addition, the birth-store rule:
//!   assignments whose target root is a field of `this` are permitted —
//!   the instance under construction is unreachable from any other
//!   holder until the constructor returns.
//! cites: 22.15
//! expect: accept
module Conf;

public class Point3
  private _x: int;
  private _y: int;

  public pure constructor(x: int, y: int)
    _x := x;
    _y := y;
  end constructor;

  public pure method X(): int => _x;
end Point3;
