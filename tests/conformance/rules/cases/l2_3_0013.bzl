//! case: l2_3_0013
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a uint constant with a toBase is ambiguous and refused
//! rule: stdlib §2.3: "A `char` and the integer types other than `int` and `long`
//!     each convert to two or more of these parameter types and match none
//!     exactly, so the call is ambiguous and a compile error (§22.6)". A `uint`
//!     constant reaches the `int` form by §5.1's constant narrowing, which
//!     "is graded as any other conversion is, never as an exact match", and
//!     reaches the `long` form by the admitted `uint` → `long` widening; the
//!     two candidates therefore both convert at the value position and are
//!     equally ranked there (§22.6), so the call is ambiguous.
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(5U, 10));
  end Main;
end Program;
