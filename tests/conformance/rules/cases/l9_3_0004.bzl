//! case: l9_3_0004
//! doc: stdlib
//! section: 9.3
//! title: a decimal renders without trailing zeros because scale is not part of its value
//! rule: stdlib §9.3 (v0.345) "Rendering and range": "There is no exponent, no grouping
//!   separator, and no trailing-zero padding, because **scale is not part of a `decimal`'s
//!   value**: `1.50m` and `1.5m` are one value and both render `1.5`".
//! cites: 9.3
//! expect: run
//! stdout: 1.5
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(1.50m);
  end Main;
end Program;
