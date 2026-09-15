//! case: s3_6_0006
//! doc: spec
//! section: 3.6
//! title: auto-hashable records get a synthesized structural equality operator
//! rule: auto-hashable classifies records and value-like choices for which =,
//!   GetHashCode, and ToString are synthesized.
//! cites: 3.6, 9.2
//! expect: run
//! stdout: True
module Conf;

public record Pair(X: int, Y: int);

public class Program
  public static method Main()
    Console.WriteLine($"{new Pair(1, 2) = new Pair(1, 2)}");
  end Main;
end Program;
