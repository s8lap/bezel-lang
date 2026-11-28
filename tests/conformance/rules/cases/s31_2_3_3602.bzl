//! case: s31_2_3_3602
//! doc: spec
//! section: 31.2.3
//! title: an interface arm before a bare Exception arm is reachable
//! rule: an earlier unguarded bare `Exception` arm covers every later arm, interface arms included
//!   (`var ex: Exception => …, IFailure => ()` draws the warning at the `IFailure` arm), because every
//!   throwable is an `Exception`
//! cites: 31.2.3, 31.1, 31.4
//! expect: quiet
//! issue: 8425
//! notes: Twin of s31_2_3_3601: the one edit is the arm order.
module Conf;

public class Program
  public static method Run()
    try
      Console.WriteLine(int.Parse("x"));
    catch
      IFailure => (),
      var ex: Exception => Console.WriteLine(ex.Message)
    end;
  end Run;
end Program;
