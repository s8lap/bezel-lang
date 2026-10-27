//! case: l9_6_0014
//! doc: stdlib
//! section: 9.6
//! title: PadCenter pads with the given padding character
//! rule: public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//!   Pad contract (all three Pad members): totalWidth <= Length returns the
//!   original string unchanged; totalWidth < 0 raises
//!   ArgumentOutOfRangeException. PadCenter splits the padding evenly.
//! cites: 9.6
//! expect: run
//! stdout: [*ab**]
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("[" + "ab".PadCenter(5, '*') + "]");
  end Main;
end Program;
