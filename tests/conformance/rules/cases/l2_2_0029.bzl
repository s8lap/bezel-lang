//! case: l2_2_0029
//! doc: stdlib
//! section: 2.2
//! title: Math.Floor of a byte argument is ambiguous and refused
//! rule: stdlib §2.2 declares `Floor(x: double): double` and
//!     `Floor(x: decimal): decimal`. §5.1's conversion table admits a `byte`
//!     at both `double` and `decimal`, and the argument matches neither
//!     parameter type exactly, so the two candidates are equally ranked at
//!     that position (§22.6: "two candidates that both require an implicit
//!     conversion at the same position are equally ranked there") and the call
//!     is ambiguous — "a compile error".
//! cites: 2.2, 5.1, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    var b: byte := 5;
    Console.WriteLine(Math.Floor(b));
  end Main;
end Program;
