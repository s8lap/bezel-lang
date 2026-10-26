//! case: l2_2_0020
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a long constant plus an int operand promotes to long
//! rule: stdlib §2.2 declares `Abs(x: long): long`, and the `long` argument is an
//!     exact match for it (§22.6), so the result is a `long`. §6.2's mixed-type
//!     arithmetic: "the smaller-range operand is implicitly promoted to the larger
//!     type ... The result has the larger type" - so the sum is 4000000000, a value
//!     that does not fit an `int`.
//! cites: 2.2, 6.2, 22.6
//! expect: run
//! stdout: 4000000000
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Abs(2000000000L) + 2000000000);
  end Main;
end Program;
