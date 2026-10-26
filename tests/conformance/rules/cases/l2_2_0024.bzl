//! case: l2_2_0024
//! doc: stdlib
//! section: 2.2
//! title: Math.Round admits a constant uint argument at its int digits parameter
//! rule: stdlib §2.2 declares `Round(x: double, digits: int := 0): double`. §5.1's
//!     constant narrowing is value-based and works "across signedness" - "a
//!     `uint`-typed constant `7` is admissible where `int` is expected" - so the
//!     `uint` constant `2U` reaches the `digits: int` parameter.
//! cites: 2.2, 5.1
//! expect: run
//! stdout: 3.14
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Round(3.14159, 2U));
  end Main;
end Program;
