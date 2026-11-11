//! case: s31_4_0009
//! doc: spec
//! section: 31.4
//! title: a user class listing IFailure is a failure an IFailure arm takes
//! rule: User code chooses per class: `class ConfigError(message: string):
//!   Exception(message), IFailure` declares a failure, and a class that does
//!   not list the marker makes no statement. … The marker is read in two
//!   places. A catch arm that names it selects on it: an `IFailure` arm takes
//!   every failure and lets every other exception, cancellation included, pass
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
