//! case: l2_3_0012
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a byte value with a toBase is ambiguous and refused
//! rule: stdlib §2.3: "A `char` and the integer types other than `int` and `long`
//!     each convert to two or more of these parameter types and match none
//!     exactly, so the call is ambiguous and a compile error (§22.6)". A `byte`
//!     reaches `int`, `long`, `float`, `double` and `decimal` (§5.1's conversion
//!     table) and matches neither of the two-argument forms' value parameters
//!     (`int` and `long`) exactly, so the two-argument `ToString` call on a
//!     `byte` is ambiguous.
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var b: byte := 5;
    Console.WriteLine(Convert.ToString(b, 10));
  end Main;
end Program;
