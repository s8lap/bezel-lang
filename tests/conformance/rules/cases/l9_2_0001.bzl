//! case: l9_2_0001
//! doc: stdlib
//! section: 9.2
//! title: double.IsNaN reports the NaN constant true
//! rule: static pure method IsNaN(value: double): bool;
//! cites: 9.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(double.IsNaN(double.NaN));
  end Main;
end Program;
