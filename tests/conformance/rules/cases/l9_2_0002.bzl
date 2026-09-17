//! case: l9_2_0002
//! doc: stdlib
//! section: 9.2
//! title: double.IeeeEquals(NaN, NaN) is false
//! rule: IeeeEquals returns false for any pair involving NaN, including
//!   NaN-vs-NaN.
//! cites: 9.2
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(double.IeeeEquals(double.NaN, double.NaN));
  end Main;
end Program;
