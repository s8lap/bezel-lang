//! case: s19_6_0009
//! doc: spec
//! section: 19.6
//! title: a case whose every arm is guarded is never statically exhaustive
//! rule: An arm carrying a `when` guard contributes nothing to static
//!   exhaustiveness. A match whose every arm is guarded is therefore never
//!   statically exhaustive — in expression position it draws the warning and
//!   raises `MatchFailureException` if every guard declines at run time.
//! cites: 19.4
//! expect: run
//! warns: yes
//! stdout: no match
module Conf;

public class Program
  public static method Main()
    var n := 5;
    try
      var label := (case n of
        var t when t > 10 => "big",
        var t when t < 0 => "negative");
      Console.WriteLine(label);
    catch
      var ex: MatchFailureException => Console.WriteLine("no match");
    end try;
  end Main;
end Program;
