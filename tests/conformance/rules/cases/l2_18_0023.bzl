//! case: l2_18_0023
//! doc: stdlib
//! section: 2.18
//! title: a floating specifier renders a non-finite double as its name, with no percent suffix
//! rule: stdlib §2.18's non-finite paragraph: "every floating specifier (`F`,
//!     `E`, `G` and `P`, with or without a digit count, and `R`) render a NaN
//!     as `NaN`, positive infinity as `Infinity` and negative infinity as
//!     `-Infinity` ... with no digits, no exponent and no `%` suffix". The `P`
//!     specifier of an infinity carries no `%` suffix, so `$"{1.0 / 0.0:P2}"`
//!     renders `Infinity`.
//! cites: 2.18, 18.5
//! expect: run
//! stdout: Infinity
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{1.0 / 0.0:P2}");
  end Main;
end Program;
