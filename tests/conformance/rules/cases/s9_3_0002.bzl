//! case: s9_3_0002
//! doc: spec
//! section: 9.3
//! title: a combined-form body may not repeat the auto-generated primary constructor's parameter list
//! rule: The body may not declare a constructor with the same parameter list as
//!   the auto-generated primary constructor.
//! expect: reject
//! diagnostic: primary constructor
module Conf;

public record Point(X: int, Y: int)
  public constructor(x: int, y: int)
    X := x;
    Y := y;
  end constructor;
end Point;
