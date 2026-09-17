//! case: l8_1_0002
//! doc: stdlib
//! section: 8.1
//! title: StartNew() returns an already-running Stopwatch
//! rule: static method StartNew(): owned Stopwatch; ... StartNew() is
//!   the fresh-instance form.
//! cites: 8.1
//! expect: run
//! stdout: True
module Conf;

import System.Diagnostics;

public class Program
  public static method Main()
    using var sw := Stopwatch.StartNew();
    Console.WriteLine(sw.IsRunning);
  end Main;
end Program;
