//! case: l2_3_0010
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a uint variable is ambiguous and refused
//! rule: stdlib §2.3: "A `char` and the integer types other than `int` and `long`
//!     each convert to two or more of these parameter types and match none
//!     exactly, so the call is ambiguous and a compile error (§22.6)". A `uint`
//!     reaches `long`, `float`, `double` and `decimal` (§5.1's conversion table)
//!     and matches none of the one-argument forms exactly, so the one-argument
//!     `ToString` call on a `uint` variable is ambiguous.
//! cites: 2.3, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var u: uint := 5U;
    Console.WriteLine(Convert.ToString(u));
  end Main;
end Program;
