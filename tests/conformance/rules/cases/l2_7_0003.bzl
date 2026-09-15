//! case: l2_7_0003
//! doc: stdlib
//! section: 2.7
//! title: Next(maxValue) rejects a negative maxValue
//! rule: Random.Next(maxValue) raises ArgumentOutOfRangeException when
//!   maxValue < 0.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    try
      _ := r.Next(-5);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
