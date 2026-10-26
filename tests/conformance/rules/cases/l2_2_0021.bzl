//! case: l2_2_0021
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a long local plus an int operand promotes to long
//! rule: stdlib §2.2 declares `Abs(x: long): long`; a `long` argument is an exact match
//!     for it whatever its provenance (§22.6), so the result is a `long` and §6.2's
//!     promotion makes the sum 4000000000. This is the non-constant twin of
//!     l2_2_0020, which differs by the argument's provenance alone.
//! cites: 2.2, 6.2, 22.6
//! expect: run
//! stdout: 4000000000
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    var x: long := 2000000000L;
    Console.WriteLine(Math.Abs(x) + 2000000000);
  end Main;
end Program;
