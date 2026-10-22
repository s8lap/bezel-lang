//! case: s22_6_0107
//! doc: spec
//! section: 22.6
//! title: a uint argument is ambiguous among the long, double and decimal Math.Abs overloads
//! rule: "two candidates that both require an implicit conversion at the same position are
//!   **equally ranked** there, regardless of which conversion each uses" (§22.6, v0.340);
//!   the stdlib declares `Math.Abs` for exactly `int`, `long`, `double` and `decimal`
//!   (stdlib §2.2). A `uint` converts to `long`, `double` and `decimal` but not `int`
//!   (§5.1); the three remaining candidates all convert and none is exact.
//!   The call is therefore ambiguous.
//! cites: 5.1
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var x: uint := 4000000000U;
    Console.WriteLine(Math.Abs(x));
  end Main;
end Program;
