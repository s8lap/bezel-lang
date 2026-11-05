//! case: l2_18_0902
//! doc: stdlib
//! section: 2.18
//! title: a decimal percent whose dropped digit is an exact 5 after an odd digit rounds up, with carry
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 14%
//!   2%
//!   100%
//! issue: 2253
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.135m:P0}");
    Console.WriteLine($"{0.015m:P0}");
    Console.WriteLine($"{0.995m:P0}");
  end Main;
end Program;
