//! case: s35_3_0032
//! doc: spec
//! section: 35.3
//! title: a qualified payload-free case written with an empty argument list in an expression body is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The qualified construction of a payload-free
//!   case is the case value itself; an argument list on it is the §35.5 arity error.
//! cites: 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Fetch(): Outcome => Outcome.Nothing();   // ERROR: the case is a value, not a call
end Program;
