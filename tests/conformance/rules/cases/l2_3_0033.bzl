//! case: l2_3_0033
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToInt32 out of range raises ParseRangeException, which an IFailure arm takes
//! rule: and `ParseRangeException` (§9.1; an `OverflowException`) when the value
//!   does not fit — the same contract as the §9 per-type `Parse` family these
//!   conversions mirror
//! cites: 2.3, 9.1
//! expect: run
//! stdout: True
//! issue: 8417
module Conf;

public class Program
  public static method Main()
    try
      _ := Convert.ToInt32("99999999999");
    catch
      var f: IFailure => Console.WriteLine(f is ParseRangeException)
    end;
  end Main;
end Program;
