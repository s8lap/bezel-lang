//! case: l1_0001
//! doc: stdlib
//! section: 1
//! title: ReadKey is the assert-shaped throwing tier at end of input, and raises a failure
//! rule: `ReadKey` remains the assert-shaped throwing tier: a caller that does not
//!   expect end of input handles nothing, and the `EndOfStreamException` then
//!   raised is a failure (§1), end of input being the environment's and not the
//!   program's.
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
