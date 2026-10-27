//! case: l9_6_0013
//! doc: stdlib
//! section: 9.6
//! title: PadCenter returns the string unchanged when the total width is its length
//! rule: Pad contract (all three Pad members): totalWidth <= Length returns the
//!   original string unchanged; totalWidth < 0 raises
//!   ArgumentOutOfRangeException.
//! cites: 9.6
//! expect: run
//! stdout: [ab]
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("[" + "ab".PadCenter(2) + "]");
  end Main;
end Program;
