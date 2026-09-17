//! case: s19_6_0031
//! doc: spec
//! section: 19.6
//! title: a total positional destructure over a non-nullable record needs no wildcard arm
//! rule: "**Record scrutinee:** the scrutinee's static type is a non-nullable
//!   record type and some unguarded arm matches every value of it — a
//!   positional or named destructuring pattern whose head names that type and
//!   every of whose slots is irrefutable. A record has exactly one shape and is
//!   implicitly sealed, so covering that shape covers every value; no `_` arm
//!   is required."
//! cites: 19.6, 19.3, 9.6
//! expect: quiet
//! issue: 4372
module Conf;

public record Point(X: int, Y: int);

public class Program
  public method Sum(p: Point): int => (case p of
    Point(var x, var y) => x + y);
end Program;
