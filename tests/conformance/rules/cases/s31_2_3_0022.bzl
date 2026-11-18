//! case: s31_2_3_0022
//! doc: spec
//! section: 31.2.3
//! title: an EndOfStreamException arm after an IFailure arm is unreachable
//! rule: An arm is unreachable when an earlier, unguarded arm already matches
//!   every exception it could match — each type the later arm names being the
//!   earlier arm's type, a class deriving from it, or a class implementing an
//!   interface type the earlier arm names … All derive from `IOException`,
//!   which derives from `Exception` and implements `IFailure` (language spec
//!   §31.4), so every member of the family is a failure.
//! cites: 31.2.3, 31.4
//! expect: warn
//! diagnostic: unreachable
//! issue: 8410
module Conf;

import System.IO;

public class Reader
  public method Read()
    try
      _ := Console.ReadKey();
    catch
      IFailure => Console.WriteLine("failure"),
      EndOfStreamException => Console.WriteLine("end of input")
    end;
  end Read;
end Reader;
