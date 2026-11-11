//! case: s22_5_4603
//! doc: spec
//! section: 22.5
//! title: a reordered named call of Debug.Assert binds condition and message by name
//! rule: Named arguments may appear in any order after positional arguments.
//! cites: 8.2
//! expect: run
//! stdout: ok
//! issue: 7602
module Conf;

import System.Diagnostics;

public class Program
  public static method Main()
    Debug.Assert(message := "never shown", condition := true);
    Console.WriteLine("ok");
  end Main;
end Program;
