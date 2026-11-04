//! case: l2_2_0040
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a float inside a lambda is a double, so a (float) -> double lambda is well typed
//! rule: public static pure method Abs(x: int): int; public static pure method
//!   Abs(x: long): long; public static pure method Abs(x: double): double;
//!   public static pure method Abs(x: decimal): decimal;
//! cites: 5.1, 22.6
//! expect: run
//! output: 2.5
//!   1.5
//! notes: A float converts only to double among the four Abs parameter types (spec 5.1: the float row is double alone), so Abs(float) is the sole applicable candidate and the call has type double. The lambda body therefore has type double, matching the declared (float) -> double. The values -2.5 and -1.5 are exact in float, so the double rendering is the shortest round-trip string.
module Conf;

public class Program
  public static method Main()
    var h: (float) -> double := lambda(x: float) => Math.Abs(x);
    Console.WriteLine(h(-2.5f));
    var k := lambda(x: float) => Math.Abs(x);
    var kd: double := k(-1.5f);
    Console.WriteLine(kd);
  end Main;
end Program;
