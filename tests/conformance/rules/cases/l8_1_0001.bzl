//! case: l8_1_0001
//! doc: stdlib
//! section: 8.1
//! title: Reset() stops the stopwatch
//! rule: Reset() stops the stopwatch and zeroes Elapsed; a restart from
//!   zero is sw.Reset(); sw.Start();
//! cites: 8.1
//! expect: run
//! stdout: False
module Conf;

import System.Diagnostics;

public class Program
  public static method Main()
    using var sw := Stopwatch.StartNew();
    sw.Reset();
    Console.WriteLine(sw.IsRunning);
  end Main;
end Program;
