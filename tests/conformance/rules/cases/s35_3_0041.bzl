//! case: s35_3_0041
//! doc: spec
//! section: 35.3
//! title: the same match arm with the payload-free case written as a value is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call).
//! cites: 35.5
//! expect: run
//! output: none
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Got("a");
    var s := (case o of Got(var b) => Outcome.Nothing, Nothing => Outcome.Nothing);
    Console.WriteLine((case s of Got(var c) => c, Nothing => "none"));
  end Main;
end Program;
