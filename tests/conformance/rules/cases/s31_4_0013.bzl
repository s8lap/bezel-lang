//! case: s31_4_0013
//! doc: spec
//! section: 31.4
//! title: a subclass of a marked class is a failure without listing IFailure
//! rule: A class implements it by listing it, and a class deriving from one that
//!   implements it implements it too (§14.2 rule 1), so a kind is inherited and
//!   cannot be shed.
//! cites: 31.4, 14.2
//! expect: run
//! stdout: inherited failure
//! issue: 8414
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class MissingSetting(message: string): ConfigError(message)
end MissingSetting;

public class Program
  public static method Main()
    try
      throw new MissingSetting("no port");
    catch
      IFailure => Console.WriteLine("inherited failure")
    end;
  end Main;
end Program;
