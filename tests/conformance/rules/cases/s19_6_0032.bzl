//! case: s19_6_0032
//! doc: spec
//! section: 19.6
//! title: an exact-type pattern over a non-nullable record scrutinee is itself total
//! rule: A `case` over a non-nullable record scrutinee is statically
//!   exhaustive with no `_` arm when some unguarded arm is a type pattern
//!   naming that exact type — §19.3: over a non-nullable value scrutinee a
//!   type pattern naming its exact type always matches.
//! cites: 19.6, 19.3, 9.6
//! expect: quiet
//! issue: 4372
module Conf;

public record Point(X: int, Y: int);

public class Program
  public method FirstOf(p: Point): int => (case p of
    var q: Point => q.X);
end Program;
