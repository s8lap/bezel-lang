//! case: l2_3_0021
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a uint value with a toBase widens to the long form
//! rule: stdlib §2.3 declares `ToString(value: int, toBase: int)` and
//!     `ToString(value: long, toBase: int)`, and "where that selects an
//!     overload the text is the converted value's". §5.1 admits the
//!     `uint` → `long` widening and never the `uint` → `int` conversion, so
//!     exactly one candidate is applicable and the value is rendered in the
//!     radix, in §9.1's lower-case digits.
//! cites: 2.3, 5.1, 9.1, 22.6
//! expect: run
//! stdout: ee6b2800
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000U;
    Console.WriteLine(Convert.ToString(u, 16));
  end Main;
end Program;
