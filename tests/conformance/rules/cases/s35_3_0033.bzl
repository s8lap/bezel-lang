//! case: s35_3_0033
//! doc: spec
//! section: 35.3
//! title: the same expression body with the payload-free case written as a value is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call).
//! cites: 35.5
//! expect: accept
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Fetch(): Outcome => Outcome.Nothing;
end Program;
