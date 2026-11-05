//! case: l2_18_0901
//! doc: stdlib
//! section: 2.18
//! title: a decimal percent whose dropped digit is an exact 5 after an even digit rounds down
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 12%
//!   2%
//!   0%
//!   98%
//! issue: 2253
//! notes: E and the E/En exponent shape are deliberately not exercised: stdlib 2.18 does not pin the
//!   scientific rendering.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.125m:P0}");
    Console.WriteLine($"{0.025m:P0}");
    Console.WriteLine($"{0.005m:P0}");
    Console.WriteLine($"{0.985m:P0}");
  end Main;
end Program;
