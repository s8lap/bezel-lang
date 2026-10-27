//! case: l2_18_0021
//! doc: stdlib
//! section: 2.18
//! title: an interpolation hole renders a non-finite double as Infinity
//! rule: stdlib §2.18's non-finite paragraph: "The no-format `ToString()` and
//!     every floating specifier ... render a NaN as `NaN`, positive infinity as
//!     `Infinity` and negative infinity as `-Infinity` ... with no digits, no
//!     exponent and no `%` suffix: `$"{1.0 / 0.0}"` and `(1.0 / 0.0).ToString("P2")`
//!     both render `"Infinity"`." The hole dispatches to the value's own
//!     rendering (language spec §18.5), so `$"{1.0 / 0.0}"` renders `Infinity`.
//! cites: 2.18, 18.5
//! expect: run
//! stdout: Infinity
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{1.0 / 0.0}");
  end Main;
end Program;
