//! case: s22_5_0018
//! doc: spec
//! section: 22.5
//! title: a reordered named argument on a built-in member binds to the parameter it names
//! rule: §22.5, binding: each supplied argument binds exactly one parameter, and
//!   naming an argument changes which parameter it binds to, never when it runs -
//!   so a reordered named argument on a built-in member selects the parameter it
//!   names. stdlib §2.2 declares `Pow(x: double, y: double)`, `Fmod(x: double, y:
//!   double)`, `Clamp(value: T, low: T, high: T)`, `Min<T>(a: T, b: T)` and
//!   `Max<T>(a: T, b: T)`, and stdlib §2.3 declares `ToString(value: int, toBase:
//!   int)`; each reordered spelling therefore computes the same value as the
//!   positional one.
//! cites: 22.5, 2.2, 2.3
//! expect: run
//! output: 9
//!   2
//!   10
//!   1
//!   2
//!   ff
//! issue: 7199
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Pow(y := 2, x := 3).ToString());
    Console.WriteLine(Math.Fmod(y := 4, x := 10).ToString());
    Console.WriteLine(Math.Clamp(high := 10, low := 0, value := 100).ToString());
    Console.WriteLine(Math.Min(b := 2, a := 1).ToString());
    Console.WriteLine(Math.Max(b := 2, a := 1).ToString());
    Console.WriteLine(Convert.ToString(toBase := 16, value := 255));
  end Main;
end Program;
