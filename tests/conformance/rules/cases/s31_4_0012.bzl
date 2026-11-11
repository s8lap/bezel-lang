//! case: s31_4_0012
//! doc: spec
//! section: 31.4
//! title: the FormatException int.Parse raises passes an IFailure arm
//! rule: No built-in type implements `IFailure`. … The marker is read in two
//!   places. A catch arm that names it selects on it: an `IFailure` arm takes
//!   every failure and lets every other exception, cancellation included, pass
//! cites: 31.4, 31.2.1
//! expect: run
//! stdout: format passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        _ := int.Parse("12x");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      FormatException => Console.WriteLine("format passed")
    end;
  end Main;
end Program;
