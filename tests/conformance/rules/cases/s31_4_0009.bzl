//! case: s31_4_0009
//! doc: spec
//! section: 31.4
//! title: a user class listing IFailure is a failure an IFailure arm takes
//! rule: User code chooses per class: `class ConfigError(message: string):
//!   Exception(message), IFailure` declares a failure, and a class that does
//!   not list the marker declares a defect report. … A catch arm that names the
//!   marker selects on it: an `IFailure` arm takes every failure and lets
//!   defects and cancellation pass
//! cites: 31.4, 31.2.1
//! expect: run
//! stdout: failure
//! issue: 8414
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Main()
    try
      throw new ConfigError("port is not a number");
    catch
      IFailure => Console.WriteLine("failure")
    end;
  end Main;
end Program;
