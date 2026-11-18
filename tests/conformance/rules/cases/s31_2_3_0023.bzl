//! case: s31_2_3_0023
//! doc: spec
//! section: 31.2.3
//! title: an EndOfStreamException arm before an IFailure arm is reachable
//! rule: An arm is unreachable when an earlier, unguarded arm already matches
//!   every exception it could match — each type the later arm names being the
//!   earlier arm's type, a class deriving from it, or a class implementing an
//!   interface type the earlier arm names
//! cites: 31.2.3, 31.4
//! expect: quiet
//! issue: 8414
module Conf;

import System.IO;

public class Reader
  public method Read()
    try
      _ := Console.ReadKey();
    catch
      EndOfStreamException => Console.WriteLine("end of input"),
      IFailure => Console.WriteLine("failure")
    end;
  end Read;
end Reader;
