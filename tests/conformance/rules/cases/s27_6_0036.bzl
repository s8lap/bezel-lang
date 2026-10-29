//! case: s27_6_0036
//! doc: spec
//! section: 27.6
//! title: a bare reference to the generic Math.Sign is rejected even with a target type
//! rule: "a reference to a generic method with no list is a compile error naming the type
//!   parameters to supply, and no inference binds them from the target" (§27.6, v0.350).
//!   `Math.Sign` is `public static method Sign<T>(x: T): int where T: INumber<T>` (stdlib §2.2,
//!   v0.350), and its only overload is generic, so the target `(int) -> int` binds nothing.
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type parameter
module Conf;

public class Program
  public static method Main()
    var g: (int) -> int := Math.Sign;
    Console.WriteLine(g(-5));
  end Main;
end Program;
