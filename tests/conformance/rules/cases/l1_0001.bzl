//! case: l1_0001
//! doc: stdlib
//! section: 1
//! title: ReadKey is the throwing form at end of input, and raises an IFailure-marked exception
//! rule: `ReadKey` remains the throwing form: a caller that does not expect end of
//!   input handles nothing at the call. … raises `EndOfStreamException` (§7.5)
//!   when input has ended … The class is `IFailure`-marked (§1), end of input
//!   being the environment's and not the program's.
//! cites: 2.1
//! expect: run
//! stdout: threw
module Conf;

import System.IO;

public class Program
  public static method Main()
    try
      _ := Console.ReadKey();
      Console.WriteLine("no exception");
    catch
      EndOfStreamException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
