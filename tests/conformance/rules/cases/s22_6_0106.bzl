//! case: s22_6_0106
//! doc: spec
//! section: 22.6
//! title: a byte argument converts to every Math.Abs overload and the call is ambiguous
//! rule: "two candidates that both require an implicit conversion at the same position are
//!   **equally ranked** there, regardless of which conversion each uses" (§22.6, v0.340);
//!   the stdlib declares `Math.Abs` for exactly `int`, `long`, `double` and `decimal`
//!   (stdlib §2.2). A `byte` converts to all four (§5.1), none is exact, so the call is
//!   ambiguous.
//! cites: 5.1
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var x: byte := 200;
    Console.WriteLine(Math.Abs(x));
  end Main;
end Program;
