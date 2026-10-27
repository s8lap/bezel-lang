//! case: l2_2_0019
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a long argument resolves to the long overload and yields the long value
//! rule: stdlib §2.2 declares `Abs(x: int): int` and `Abs(x: long): long`. A `long`
//!     argument is an exact match for the `long` overload - §22.6: "an exact match
//!     is the only relation that distinguishes one applicable candidate from
//!     another at an argument position" - so the call resolves to `Abs(x: long):
//!     long` and its result is the long 2147483648, not an `int` overflow.
//! cites: 2.2, 22.6
//! expect: run
//! stdout: 2147483648
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Abs(-2147483648L));
  end Main;
end Program;
