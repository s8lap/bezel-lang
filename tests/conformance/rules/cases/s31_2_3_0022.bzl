//! case: s31_2_3_0022
//! doc: spec
//! section: 31.2.3
//! title: a FormatException arm after an IFailure arm is unreachable
//! rule: An arm is unreachable when an earlier, unguarded arm already matches every
//!   exception it could match — each type the later arm names being the earlier
//!   arm's type, a class deriving from it, or a class implementing an interface
//!   type the earlier arm names
//! cites: 31.2.3, 31.4
//! expect: warn
//! diagnostic: unreachable
//! issue: 8410
module Conf;

public class Reader
  public method Read(text: string)
    try
      Console.WriteLine(int.Parse(text));
    catch
      IFailure => Console.WriteLine("failure"),
      FormatException => Console.WriteLine("format")
    end;
  end Read;
end Reader;
