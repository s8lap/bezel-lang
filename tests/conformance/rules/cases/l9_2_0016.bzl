//! case: l9_2_0016
//! doc: stdlib
//! section: 9.2
//! title: Float.IsNaN accepts its parameter by name
//! rule: Float.IsNaN is declared IsNaN(value: float); named arguments bind to the
//!   declared parameter name (language spec 22.5), and an int converts to float
//!   implicitly (spec 5.1).
//! cites: 22.5, 5.1
//! expect: run
//! output: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Float.IsNaN(value := 3));
  end Main;
end Program;
