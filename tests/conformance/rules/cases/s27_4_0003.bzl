//! case: s27_4_0003
//! doc: spec
//! section: 27.4
//! title: a positional-record destructuring parameter binds each field by desugaring
//! rule: A lambda parameter may be a positional record destructuring;
//!   `lambda(Entry(k, v)) => ...` behaves as a lambda over the record type
//!   with each slot bound to the corresponding positional field.
//! cites: 27.4, 19.3
//! expect: run
//! stdout: 7
module Conf;

record Pair(X: int, Y: int);

public class Program
  public static method Main()
    var sum: (Pair) -> int := lambda(Pair(a, b)) => a + b;
    Console.WriteLine($"{sum(new Pair(3, 4))}");
  end Main;
end Program;
