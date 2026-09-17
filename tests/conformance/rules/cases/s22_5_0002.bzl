//! case: s22_5_0002
//! doc: spec
//! section: 22.5
//! title: a positional argument after a named argument is rejected
//! rule: Named arguments may appear in any order after positional
//!   arguments.
//! cites: 22.5
//! expect: reject
//! diagnostic: positional
module Conf;

public class Logger
  public static method Log(message: string, level: string := "INFO")
  end Log;
end Logger;

public class Program
  public static method Main()
    Logger.Log(level := "ERROR", "Failed");
  end Main;
end Program;
