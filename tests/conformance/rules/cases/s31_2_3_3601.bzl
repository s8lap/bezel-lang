//! case: s31_2_3_3601
//! doc: spec
//! section: 31.2.3
//! title: an interface arm after a bare Exception arm is unreachable
//! rule: an earlier unguarded bare `Exception` arm covers every later arm, interface arms included
//!   (`var ex: Exception => …, IFailure => ()` draws the warning at the `IFailure` arm), because every
//!   throwable is an `Exception`
//! cites: 31.2.3, 31.1, 31.4
//! expect: warn
//! diagnostic: unreachable
//! issue: 8425
//! notes: Twin s31_2_3_3602 puts the interface arm first.
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(int.Parse("x"));
    catch
      var ex: Exception => Console.WriteLine(ex.Message),
      IFailure => ()
    end;
  end Main;
end Program;
