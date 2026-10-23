//! case: s35_3_0038
//! doc: spec
//! section: 35.3
//! title: a payload-free case written with an empty argument list inside an interpolation hole is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The hole of an interpolated string is an
//!   ordinary expression position, and the case value it names takes no argument list.
//! cites: 18.5, 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    Console.WriteLine($"x{Outcome.Nothing()}");   // ERROR: the case is a value, not a call
  end Main;
end Program;
