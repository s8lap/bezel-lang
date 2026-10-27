//! case: s22_5_0019
//! doc: spec
//! section: 22.5
//! title: named arguments after a positional one bind to the parameter they name
//! rule: §22.5, mixed spellings: a call that mixes positional and named arguments
//!   binds its positional arguments to the leading parameters and each named one
//!   to the parameter it names - named arguments may appear in any order after
//!   positional arguments, and each supplied argument binds exactly one
//!   parameter. So `Clamp(100, high := 10, low := 0)` is `Clamp(value: 100, low:
//!   0, high: 10)` (stdlib §2.2) and returns 10; `Pow(3, y := 2)` is `Pow(x: 3,
//!   y: 2)`; and `ToString(255, toBase := 16)` is `ToString(value: 255, toBase:
//!   16)` (stdlib §2.3).
//! cites: 22.5, 2.2, 2.3
//! expect: run
//! output: 10
//!   9
//!   ff
//! issue: 7199
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Clamp(100, high := 10, low := 0).ToString());
    Console.WriteLine(Math.Pow(3, y := 2).ToString());
    Console.WriteLine(Convert.ToString(255, toBase := 16));
  end Main;
end Program;
