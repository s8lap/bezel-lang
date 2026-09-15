//! case: s19_6_0008
//! doc: spec
//! section: 19.6
//! title: a wildcard arm placed last is admitted and selected only when no arm matches
//! rule: If `_` appears, it must be the last arm. Arms are evaluated in textual
//!   order; the first arm whose pattern matches is selected, and an unguarded
//!   `_` is the irrefutable arm that makes the match statically exhaustive.
//! cites: 19.4
//! expect: run
//! output: one
//!   other
module Conf;

public class Program
  public static method Label(n: int): string => (case n of
    1 => "one",
    _ => "other");

  public static method Main()
    Console.WriteLine(Label(1));
    Console.WriteLine(Label(5));
  end Main;
end Program;
