//! case: s35_3_0043
//! doc: spec
//! section: 35.3
//! title: the same aliased payload-free case written as a value is admitted
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

type O := Outcome;

public class Program
  public static method Main()
    var o := O.Nothing;
    Console.WriteLine((case o of Got(var b) => b, Nothing => "none"));
  end Main;
end Program;
