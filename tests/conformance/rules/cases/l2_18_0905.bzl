//! case: l2_18_0905
//! doc: stdlib
//! section: 2.18
//! title: trailing zeros in a decimal's scale do not change a percent midpoint rounding
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 12%
//!   1.2%
//!   14%
//! issue: 2253
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.12500m:P0}");
    Console.WriteLine($"{0.012500m:P1}");
    Console.WriteLine($"{0.13500m:P0}");
  end Main;
end Program;
