//! case: s35_3_0047
//! doc: spec
//! section: 35.3
//! title: a payload-free case written with an empty argument list as an equality operand is rejected
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The comparison's right operand is an ordinary
//!   expression, and the qualified case value it names takes no argument list.
//! cites: 35.5, 35.7
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
    Console.WriteLine(o = Outcome.Nothing());   // ERROR: the case is a value, not a call
  end Main;
end Program;
