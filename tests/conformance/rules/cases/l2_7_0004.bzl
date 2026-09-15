//! case: l2_7_0004
//! doc: stdlib
//! section: 2.7
//! title: Next(minValue, maxValue) rejects minValue greater than maxValue
//! rule: Random.Next(minValue, maxValue) raises ArgumentOutOfRangeException
//!   when minValue > maxValue.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    var r := new Random(1);
    try
      _ := r.Next(10, 1);
      Console.WriteLine("no exception");
    catch
      ArgumentOutOfRangeException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
