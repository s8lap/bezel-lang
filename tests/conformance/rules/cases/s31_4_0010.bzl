//! case: s31_4_0010
//! doc: spec
//! section: 31.4
//! title: InvalidOperationException passes an IFailure arm
//! rule: A catch arm that names the marker selects on it: an `IFailure` arm takes
//!   every failure and lets defects and cancellation pass
//! cites: 31.4, 31.2.1
//! expect: run
//! stdout: defect passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        throw new InvalidOperationException("scope closed");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      InvalidOperationException => Console.WriteLine("defect passed")
    end;
  end Main;
end Program;
