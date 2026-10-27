//! case: l2_18_0012
//! doc: stdlib
//! section: 2.18
//! title: the lower-case x specifier renders lower-case hex
//! rule: stdlib §2.18's integer row: "`X`/`x` hex (upper/lower, `Xn` pads)".
//!     The `x` specifier of 255 is the lower-case `ff`.
//! cites: 2.18
//! expect: run
//! stdout: ff
//! issue: 7265
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((255).ToString("x"));
  end Main;
end Program;
