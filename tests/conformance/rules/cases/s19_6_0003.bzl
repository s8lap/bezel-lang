//! case: s19_6_0003
//! doc: spec
//! section: 19.6
//! title: an unmatched expression-position case raises MatchFailureException
//! rule: In expression position, an unmatched value raises
//!   `MatchFailureException` at runtime.
//! cites: 19.6
//! expect: run
//! stdout: no match
module Conf;

public class Program
  public static method Main()
    var n := 5;
    try
      var label := (case n of
        1 => "one",
        2 => "two");
      Console.WriteLine(label);
    catch
      var ex: MatchFailureException => Console.WriteLine("no match");
    end try;
  end Main;
end Program;
