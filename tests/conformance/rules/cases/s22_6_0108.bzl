//! case: s22_6_0108
//! doc: spec
//! section: 22.6
//! title: a ulong argument is ambiguous between the double and decimal Math.Abs overloads
//! rule: "two candidates that both require an implicit conversion at the same position are
//!   **equally ranked** there, regardless of which conversion each uses" (§22.6, v0.340);
//!   the stdlib declares `Math.Abs` for exactly `int`, `long`, `double` and `decimal`
//!   (stdlib §2.2). A `ulong` converts to `double` and `decimal` only (§5.1); both convert
//!   and neither is exact.
//!   The call is therefore ambiguous.
//! cites: 5.1
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var x: ulong := 18000000000000000000UL;
    Console.WriteLine(Math.Abs(x));
  end Main;
end Program;
