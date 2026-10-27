//! case: l2_18_0022
//! doc: stdlib
//! section: 2.18
//! title: a non-finite float renders NaN, Infinity or -Infinity
//! rule: stdlib §2.18's non-finite paragraph governs `double` and `float`
//!     alike: "Non-finite `double` and `float` values. The no-format
//!     `ToString()` and every floating specifier ... render a NaN as `NaN`,
//!     positive infinity as `Infinity` and negative infinity as `-Infinity` —
//!     the names of the `NaN`, `PositiveInfinity` and `NegativeInfinity`
//!     constants (§9.2) — with no digits, no exponent and no `%` suffix".
//! cites: 2.18, 9.2
//! expect: run
//! output: Infinity
//!   -Infinity
//!   NaN
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(float.PositiveInfinity.ToString());
    Console.WriteLine(float.NegativeInfinity.ToString());
    Console.WriteLine(float.NaN.ToString());
  end Main;
end Program;
