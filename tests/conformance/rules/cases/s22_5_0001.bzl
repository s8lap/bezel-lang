//! case: s22_5_0001
//! doc: spec
//! section: 22.5
//! title: named arguments may appear in any order
//! rule: Named arguments may appear in any order after positional arguments.
//! cites: 22.5
//! expect: run
//! stdout: ERROR: Failed
module Conf;

public class Logger
  public static method Log(message: string, level: string := "INFO", timestamp: bool := true): string =>
    $"{level}: {message}";
end Logger;

public class Program
  public static method Main()
    Console.WriteLine(Logger.Log(level := "ERROR", message := "Failed"));
  end Main;
end Program;
