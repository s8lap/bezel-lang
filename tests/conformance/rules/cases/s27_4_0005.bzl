//! case: s27_4_0005
//! doc: spec
//! section: 27.4
//! title: a destructuring parameter's head must exactly name the target's parameter type
//! rule: The head of a lambda-parameter destructuring must name a positional
//!   record type, and it must be exactly the parameter's type: when a target
//!   type supplies the parameter type, a head naming any other type is a
//!   compile error.
//! cites: 27.4, 19.3
//! expect: reject
//! diagnostic: parameter's type
module Conf;

record Pair(X: int, Y: int);
record Coord(X: int, Y: int);

public class Program
  public method Demo()
    var sum: (Pair) -> int := lambda(Coord(a, b)) => a + b;
  end Demo;
end Program;
