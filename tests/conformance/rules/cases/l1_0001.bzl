//! case: l1_0001
//! doc: stdlib
//! section: 1
//! title: ReadKey is the assert-shaped throwing tier at end of input
//! rule: Console.ReadKey() blocks for one key and raises EndOfStreamException at
//!   end of input; per the failure-signal rule, end of input is band 2's expected
//!   absence, but ReadKey stays the throwing tier because absence there is a bug
//!   at its call sites.
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
