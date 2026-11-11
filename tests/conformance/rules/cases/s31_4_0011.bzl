//! case: s31_4_0011
//! doc: spec
//! section: 31.4
//! title: OperationCanceledException passes an IFailure arm
//! rule: `OperationCanceledException` reports cancellation, the answer to a
//!   request the program itself made; no class in its tree implements
//!   `IFailure`, and a handler of failures never takes it.
//! cites: 31.4, 31.2.1
//! expect: run
//! stdout: cancellation passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      try
        throw new OperationCanceledException("stop");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      OperationCanceledException => Console.WriteLine("cancellation passed")
    end;
  end Main;
end Program;
