//! case: s27_4_0004
//! doc: spec
//! section: 27.4
//! title: a destructuring parameter slot may be the discard
//! rule: Each slot of a lambda-parameter destructuring is a naked identifier
//!   binding, a discard `_`, or a nested destructuring meeting the same rules.
//! cites: 27.4, 19.3
//! expect: run
//! stdout: 9
module Conf;

record Pair(X: int, Y: int);

public class Program
  public static method Main()
    var first: (Pair) -> int := lambda(Pair(x, _)) => x;
    Console.WriteLine($"{first(new Pair(9, 2))}");
  end Main;
end Program;
