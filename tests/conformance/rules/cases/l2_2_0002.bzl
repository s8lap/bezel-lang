//! case: l2_2_0002
//! doc: stdlib
//! section: 2.2
//! title: Clamp raises ArgumentException when low exceeds high
//! rule: Math.Clamp<T>(value, low, high) raises ArgumentException when low is
//!   greater than high.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := Math.Clamp(5, 10, 1);
      Console.WriteLine("no exception");
    catch
      ArgumentException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
