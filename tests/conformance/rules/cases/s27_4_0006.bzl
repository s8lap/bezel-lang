//! case: s27_4_0006
//! doc: spec
//! section: 27.4
//! title: a constant is a refutable element and is rejected inside a destructuring parameter
//! rule: Constants, ranges, relationals, type tests, `var`-form bindings, and
//!   alternatives are compile errors in destructuring parameter position — they
//!   are refutable, and a parameter list is not a `case`.
//! cites: 27.4, 19.3
//! expect: reject
//! diagnostic: refutable
module Conf;

record Pair(X: int, Y: int);

public class Program
  public method Demo()
    var f: (Pair) -> int := lambda(Pair(3, y)) => y;
  end Demo;
end Program;
