//! case: l9_2_0015
//! doc: stdlib
//! section: 9.2
//! title: Float.IsNaN accepts an int argument through the implicit int to float conversion
//! rule: Float.IsNaN takes a float parameter; an int converts implicitly to float
//!   (language spec 5.1 table, int row), so IsNaN(3) is admitted and 3 is not NaN.
//! cites: 5.1
//! expect: run
//! output: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Float.IsNaN(3));
  end Main;
end Program;
