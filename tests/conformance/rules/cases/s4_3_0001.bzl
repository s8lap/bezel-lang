//! case: s4_3_0001
//! doc: spec
//! section: 4.3
//! title: floating-point division by zero produces infinity rather than raising
//! rule: floating-point division by zero produces one of these [Infinity,
//!   -Infinity, NaN] rather than raising an exception.
//! cites: 4.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a := 1.0;
    var b := 0.0;
    Console.WriteLine($"{double.IsPositiveInfinity(a / b)}");
  end Main;
end Program;
