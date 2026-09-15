//! case: l2_4_0003
//! doc: stdlib
//! section: 2.4
//! title: Set raises ArgumentException for an empty variable name
//! rule: Environment.Set(name, value) raises ArgumentException when name is
//!   empty, or contains "=".
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      Environment.Set("", "1");
      Console.WriteLine("no exception");
    catch
      ArgumentException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
