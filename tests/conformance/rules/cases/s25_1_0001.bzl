//! case: s25_1_0001
//! doc: spec
//! section: 25.1
//! title: unary and binary operator overloads are admitted on a record
//! rule: Records, enums, choices and classes may overload operators; operators are
//!   declared as `public static` methods using the `operator` keyword. The unary
//!   set is `+`, `-`, `~` and the binary set includes `+`, `-`, `*`.
//! cites: 25, 25.1
//! expect: accept
module Conf;

public record Vector2(X: double, Y: double)
  public static method operator +(left: Vector2, right: Vector2): Vector2 =>
    new Vector2(left.X + right.X, left.Y + right.Y);

  public static method operator -(v: Vector2): Vector2 =>
    new Vector2(-v.X, -v.Y);

  public static method operator *(v: Vector2, scalar: double): Vector2 =>
    new Vector2(v.X * scalar, v.Y * scalar);
end Vector2;
