//! case: s19_6_0010
//! doc: spec
//! section: 19.6
//! title: an all-slots-irrefutable destructure of a record scrutinee needs no wildcard
//! rule: Record scrutinee: the scrutinee's static type is a non-nullable record
//!   type and some unguarded arm matches every value of it — a positional
//!   destructuring pattern whose head names that type and every of whose slots
//!   is irrefutable. No `_` arm is required.
//! cites: 9.6, 19.3
//! expect: quiet
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Describe(p: Point): string => (case p of
    Point(var x, var y) => $"{x},{y}");
end Program;
