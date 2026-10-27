//! case: l2_2_0022
//! doc: stdlib
//! section: 2.2
//! title: Math.Round admits a constant long argument at its int digits parameter
//! rule: stdlib §2.2 declares `Round(x: double, digits: int := 0): double`. §5.1's
//!     constant narrowing: "An expression of integer type that is a compile-time
//!     constant implicitly converts to any integer type whose range contains its
//!     value" - the constant `2L` reaches the `digits: int` parameter, and the call
//!     rounds 3.14159 to two digits.
//! cites: 2.2, 5.1
//! expect: run
//! stdout: 3.14
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Round(3.14159, 2L));
  end Main;
end Program;
