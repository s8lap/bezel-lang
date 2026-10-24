//! case: l9_6_0015
//! doc: stdlib
//! section: 9.6
//! title: PadCenter with a negative total width raises ArgumentOutOfRangeException
//! rule: Pad contract (all three Pad members): totalWidth <= Length returns the
//!   original string unchanged; totalWidth < 0 raises
//!   ArgumentOutOfRangeException.
//! cites: 9.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := "ab".PadCenter(-1);
    catch
      var ex: ArgumentOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
