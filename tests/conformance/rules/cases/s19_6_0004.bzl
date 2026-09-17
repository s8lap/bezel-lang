//! case: s19_6_0004
//! doc: spec
//! section: 19.6
//! title: an enum case covering every declared constant needs no wildcard
//! rule: "**Enum scrutinee:** every declared constant has a matching
//!   pattern — a constant pattern naming it directly, a var binding, or
//!   `_`. A statically exhaustive enum case cannot raise
//!   MatchFailureException."
//! cites: 19.6
//! expect: run
//! stdout: medium
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Label(p: Priority): string => (case p of
    Low    => "low",
    Medium => "medium",
    High   => "high");

  public static method Main()
    Console.WriteLine(Label(Priority.Medium));
  end Main;
end Program;
