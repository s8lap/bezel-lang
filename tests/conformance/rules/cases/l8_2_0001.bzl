//! case: l8_2_0001
//! doc: stdlib
//! section: 8.2
//! title: Debug.Assert never throws — a true condition just continues
//! rule: Assert never throws; a failed assertion terminates the
//!   process, so a true condition simply continues execution.
//! cites: 8.2
//! expect: run
//! stdout: ok
module Conf;

import System.Diagnostics;

public class Program
  public static method Main()
    Debug.Assert(true);
    Console.WriteLine("ok");
  end Main;
end Program;
