//! case: l2_2_0023
//! doc: stdlib
//! section: 2.2
//! title: Math.Round refuses a non-constant long argument at its int digits parameter
//! rule: stdlib §2.2 declares `Round(x: double, digits: int := 0): double`. §5.1's
//!     constant narrowing "applies to constants only" - "for non-constant
//!     expressions the conversion table and the never-implicit list above govern
//!     unchanged" - and the table admits no `long` to `int` row, so the sole
//!     candidate of §22.6 is inapplicable and the call is a compile error. This is
//!     the non-constant twin of l2_2_0022, which differs by the argument's
//!     provenance alone.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: must be an `int`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    var d: long := 2;
    Console.WriteLine(Math.Round(3.14159, d));
  end Main;
end Program;
