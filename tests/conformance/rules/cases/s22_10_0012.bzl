//! case: s22_10_0012
//! doc: spec
//! section: 22.10
//! title: primary constructor parameters are not automatically promoted to public properties
//! rule: Primary-constructor parameters are not automatically promoted to
//!   public properties. To expose one, declare a property explicitly.
//! cites: 22.10
//! expect: reject
//! diagnostic: member
module Conf;

public class Point(x: int, y: int)
  private _x: int := x;
  private _y: int := y;
end Point;

public class Coordinate
  public static method Sum(p: Point): int => p.x + p.y;
end Coordinate;
