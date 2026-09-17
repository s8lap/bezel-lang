//! case: s4_3_0002
//! doc: spec
//! section: 4.3
//! title: zero divided by zero produces NaN
//! rule: floating-point division by zero produces one of Infinity,
//!   -Infinity, or NaN rather than raising an exception.
//! cites: 4.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a := 0.0;
    var b := 0.0;
    Console.WriteLine($"{double.IsNaN(a / b)}");
  end Main;
end Program;
