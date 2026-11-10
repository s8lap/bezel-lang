//! case: s31_2_1_0010
//! doc: spec
//! section: 31.2.1
//! title: a binding over an IFailure arm reads Message
//! rule: a binding arm over an interface binds `name` at `Exception` … The binding
//!   therefore reads `Message`, is an argument where an `Exception` is
//!   expected, and chains as a cause (§31.4), whatever interface selected it.
//! cites: 31.2.1, 31.4
//! expect: run
//! stdout: port is not a number
//! issue: 8415
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Main()
    try
      throw new ConfigError("port is not a number");
    catch
      var f: IFailure => Console.WriteLine(f.Message)
    end;
  end Main;
end Program;
