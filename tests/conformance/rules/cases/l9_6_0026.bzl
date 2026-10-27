//! case: l9_6_0026
//! doc: stdlib
//! section: 9.6
//! title: string.PadLeft and string.PadRight with a negative total width raise ArgumentOutOfRangeException
//! rule: Pad contract (all three Pad members): totalWidth <= Length returns the
//!   original string unchanged; totalWidth < 0 raises
//!   ArgumentOutOfRangeException.
//! cites: 9.6
//! expect: run
//! output: PadLeft: caught
//!   PadRight: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := "ab".PadLeft(-1);
    catch
      var ex: ArgumentOutOfRangeException => Console.WriteLine("PadLeft: caught");
    end try;
    try
      _ := "ab".PadRight(-1);
    catch
      var ex: ArgumentOutOfRangeException => Console.WriteLine("PadRight: caught");
    end try;
  end Main;
end Program;
