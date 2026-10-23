//! case: s35_3_0046
//! doc: spec
//! section: 35.3
//! title: the same nullable choice initialized from the payload-free case value is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The value converts into the nullable form
//!   by §5.1's `T → T?` wrapping.
//! cites: 35.9
//! expect: run
//! output: none
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o: Outcome? := Outcome.Nothing;
    if o = null then
      Console.WriteLine("null");
    else
      Console.WriteLine((case o of Got(var b) => b, Nothing => "none"));
    end;
  end Main;
end Program;
