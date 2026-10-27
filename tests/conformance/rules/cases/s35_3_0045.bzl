//! case: s35_3_0045
//! doc: spec
//! section: 35.3
//! title: a payload-free case written with an empty argument list as an initializer is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). A nullable choice's declared form does not
//!   make its case value callable.
//! cites: 35.9, 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o: Outcome? := Outcome.Nothing();   // ERROR: the case is a value, not a call
  end Main;
end Program;
