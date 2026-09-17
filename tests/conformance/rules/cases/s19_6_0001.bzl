//! case: s19_6_0001
//! doc: spec
//! section: 19.6
//! title: a non-exhaustive expression-position case draws the exhaustiveness warning
//! rule: In expression position, an unmatched value raises
//!   `MatchFailureException` at runtime; the compiler warns when it cannot
//!   prove exhaustiveness statically.
//! cites: 19.6
//! expect: warn
//! diagnostic: exhaustive
module Conf;

public class Program
  public static method Main()
    var score := 55;
    var grade := (case score of
      90..100 => "A",
      80..<90 => "B");
    Console.WriteLine(grade);
  end Main;
end Program;
