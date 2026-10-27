//! case: l2_18_0013
//! doc: stdlib
//! section: 2.18
//! title: the X specifier pads to the stated digit count
//! rule: stdlib §2.18's integer row: "`X`/`x` hex (upper/lower, `Xn` pads)".
//!     `X4` pads 255 to four hex digits.
//! cites: 2.18
//! expect: run
//! stdout: 00FF
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((255).ToString("X4"));
  end Main;
end Program;
