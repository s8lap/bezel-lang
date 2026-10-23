//! case: s35_3_0050
//! doc: spec
//! section: 35.3
//! title: the same ascribed lambda body with the payload-free case written as a value is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call).
//! cites: 27.3
//! expect: run
//! output: none
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var f := lambda(): Outcome => Outcome.Nothing;
    Console.WriteLine((case f() of Got(var b) => b, Nothing => "none"));
  end Main;
end Program;
