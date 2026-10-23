//! case: s35_3_0042
//! doc: spec
//! section: 35.3
//! title: a payload-free case reached through a type alias and written with an empty argument list is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). A type alias names the same choice, and the
//!   case value reached through it takes no argument list either.
//! cites: 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

type O := Outcome;

public class Program
  public static method Main()
    var o := O.Nothing();   // ERROR: `Outcome.Nothing` is a value, not a call
  end Main;
end Program;
