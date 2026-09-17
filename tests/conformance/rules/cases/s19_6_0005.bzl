//! case: s19_6_0005
//! doc: spec
//! section: 19.6
//! title: a choice case covering every declared case needs no wildcard
//! rule: "**Choice scrutinee:** every declared case is covered by an
//!   unguarded arm — a payload-free case by a constant-style pattern; a
//!   payload-carrying case by a positional destructuring pattern whose
//!   every slot is irrefutable. When every case is covered, no `_` arm is
//!   required."
//! cites: 19.6, 35.6
//! expect: run
//! stdout: value 7
module Conf;

public choice Outcome
  Value(n: int),
  Missing;
end Outcome;

public class Program
  public static method Describe(o: Outcome): string => (case o of
    Value(var n) => $"value {n}",
    Missing      => "missing");

  public static method Main()
    Console.WriteLine(Describe(Outcome.Value(7)));
    Console.WriteLine(Describe(Outcome.Missing));
  end Main;
end Program;
