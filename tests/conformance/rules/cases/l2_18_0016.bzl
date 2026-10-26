//! case: l2_18_0016
//! doc: stdlib
//! section: 2.18
//! title: a non-finite double renders NaN, Infinity or -Infinity
//! rule: stdlib §2.18: "The no-format `ToString()` and every floating specifier
//!     ... render a NaN as `NaN`, positive infinity as `Infinity` and negative
//!     infinity as `-Infinity` — the names of the `NaN`, `PositiveInfinity` and
//!     `NegativeInfinity` constants (§9.2) — with no digits, no exponent and no
//!     `%` suffix".
//! cites: 2.18, 9.2
//! expect: run
//! output: Infinity
//!   -Infinity
//!   NaN
//! issue: 7301
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(double.PositiveInfinity.ToString());
    Console.WriteLine(double.NegativeInfinity.ToString());
    Console.WriteLine(double.NaN.ToString());
  end Main;
end Program;
