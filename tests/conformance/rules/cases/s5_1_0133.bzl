//! case: s5_1_0133
//! doc: spec
//! section: 5.1
//! title: a double argument does not convert to decimal at an explicitly bound Math.Max
//! rule: The never-implicit list in §5.1 (v0.340) includes "Between `decimal` and binary floats
//!   (`float`, `double`)". `Math.Max<decimal>(dd, 2.5)` passes a `double` literal where
//!   `decimal` is required. The twin is `2.5m`.
//! cites: 22.6
//! expect: reject
//! diagnostic: implicit
module Conf;

public class Program
  public static method Main()
    var dd: decimal := 7.5m;
    Console.WriteLine(Math.Max<decimal>(dd, 2.5));
  end Main;
end Program;
