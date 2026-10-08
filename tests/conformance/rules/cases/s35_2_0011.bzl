//! case: s35_2_0011
//! doc: spec
//! section: 35.2
//! title: an override Equals taking the choice's own type is admitted
//! rule: `override` admitted exactly where §10.5 admits it on an enum —
//!   `Equals` taking the choice's own type, `GetHashCode()` and `ToString()`
//! cites: 35.2, 10.5, 8.3
//! expect: accept
module Conf;

public choice Shape
  Circle(radius: int),
  Empty;

  public override method Equals(other: Shape): bool => (case this of
    Circle(var a) => (case other of Circle(var b) => a = b, Empty => false),
    Empty         => (case other of Empty => true, Circle(_) => false));
  public override method GetHashCode(): int => (case this of Circle(var r) => r, Empty => 0);
end Shape;
