//! case: l2_3_0009
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a uint argument is ambiguous and refused
//! rule: stdlib §2.3: "A `char` and the integer types other than `int` and `long`
//!     each convert to two or more of these parameter types and match none
//!     exactly, so the call is ambiguous and a compile error (§22.6)" - and §5.1's
//!     constant narrowing reaches a candidate only as a conversion, "never as an
//!     exact match". A `uint` is an integer type other than `int` and `long`, so
//!     `ToString(5U)` is ambiguous and refused.
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(5U));
  end Main;
end Program;
