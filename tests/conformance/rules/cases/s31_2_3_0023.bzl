//! case: s31_2_3_0023
//! doc: spec
//! section: 31.2.3
//! title: a FormatException arm before an IFailure arm is reachable
//! rule: An arm is unreachable when an earlier, unguarded arm already matches every
//!   exception it could match — each type the later arm names being the earlier
//!   arm's type, a class deriving from it, or a class implementing an interface
//!   type the earlier arm names
//! cites: 31.2.3, 31.4
//! expect: quiet
//! issue: 8414
module Conf;

public class Reader
  public method Read(text: string)
    try
      Console.WriteLine(int.Parse(text));
    catch
      FormatException => Console.WriteLine("format"),
      IFailure => Console.WriteLine("failure")
    end;
  end Read;
end Reader;
