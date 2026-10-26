//! case: l2_2_0025
//! doc: stdlib
//! section: 2.2
//! title: Math.Round refuses a constant uint argument that does not fit the int digits parameter
//! rule: stdlib §2.2 declares `Round(x: double, digits: int := 0): double`. §5.1's
//!     constant narrowing ends: "A constant whose value does not fit the target's
//!     range is a **compile error** at the conversion site - never a silent
//!     truncation." The `uint` constant 3000000000 does not fit an `int`, so the
//!     sole candidate of §22.6 is inapplicable and the call is a compile error.
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: must be an `int`
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Round(3.14159, 3000000000U));
  end Main;
end Program;
