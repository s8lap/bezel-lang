//! case: s19_6_0033
//! doc: spec
//! section: 19.6
//! title: a refutable slot leaves a record case non-exhaustive
//! rule: Record-scrutinee coverage is one level deep with **no per-slot
//!   value-space analysis** — a refutable slot (a constant, `null`, range,
//!   relational, alternative list, type test, or nested destructuring) makes
//!   the arm contribute nothing, exactly as a refutable choice-case slot does;
//!   the compiler then warns that it cannot prove exhaustiveness.
//! cites: 19.6, 19.3
//! expect: warn
//! diagnostic: exhaustive
//! issue: 4372
module Conf;

public record Point(X: int, Y: int);

public class Program
  public method YOf(p: Point): int => (case p of
    Point(0, var y) => y);
end Program;
