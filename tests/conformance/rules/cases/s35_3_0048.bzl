//! case: s35_3_0048
//! doc: spec
//! section: 35.3
//! title: the same equality against the payload-free case value is admitted and false
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The generated `=` is true exactly when both
//!   operands have the same active case and every payload compares equal (§35.7).
//! cites: 35.7
//! expect: run
//! output: False
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Got("a");
    Console.WriteLine(o = Outcome.Nothing);
  end Main;
end Program;
