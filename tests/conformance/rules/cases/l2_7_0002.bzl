//! case: l2_7_0002
//! doc: stdlib
//! section: 2.7
//! title: Next(maxValue) returns a value in 0..<maxValue
//! rule: Random.Next(maxValue) returns a value in the range 0..<maxValue.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var r := new Random(7);
    var n := r.Next(10);
    Console.WriteLine(n >= 0 and n < 10);
  end Main;
end Program;
