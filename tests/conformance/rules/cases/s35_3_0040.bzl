//! case: s35_3_0040
//! doc: spec
//! section: 35.3
//! title: a payload-free case written with an empty argument list as a match arm's result is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). An arm's result expression resolves ordinarily,
//!   and the qualified case value it names takes no argument list.
//! cites: 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Got("a");
    var s := (case o of Got(var b) => Outcome.Nothing(), Nothing => Outcome.Nothing);  // ERROR: a value, not a call
  end Main;
end Program;
