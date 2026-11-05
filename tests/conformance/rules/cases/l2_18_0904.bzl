//! case: l2_18_0904
//! doc: stdlib
//! section: 2.18
//! title: a decimal percent just above or below a midpoint is not a tie
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 13%
//!   12%
//!   15%
//!   14%
//! issue: 2253
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.12500000001m:P0}");
    Console.WriteLine($"{0.12499999999m:P0}");
    Console.WriteLine($"{0.145000001m:P0}");
    Console.WriteLine($"{0.144999999m:P0}");
  end Main;
end Program;
