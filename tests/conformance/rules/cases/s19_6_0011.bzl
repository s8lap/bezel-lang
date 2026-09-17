//! case: s19_6_0011
//! doc: spec
//! section: 19.6
//! title: a refutable slot makes a record-destructure arm contribute nothing
//! rule: Coverage is one level deep with no per-slot value-space analysis — a
//!   refutable slot (a constant, `null`, range, relational, alternative list,
//!   type test, or nested destructuring) makes the arm contribute nothing; the
//!   compiler warns when it cannot prove exhaustiveness statically.
//! cites: 19.3
//! expect: warn
//! diagnostic: exhaustive
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Describe(p: Point): string => (case p of
    Point(0, var y) => $"axis {y}");
end Program;
