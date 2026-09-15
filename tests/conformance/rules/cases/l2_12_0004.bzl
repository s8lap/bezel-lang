//! case: l2_12_0004
//! doc: stdlib
//! section: 2.12
//! title: the checked same-width cast raises where Reinterpret would not
//! rule: Reinterpret is the bit-level counterpart to the checked same-width
//!   signed<->unsigned cast: the cast performs a value conversion that raises
//!   OverflowException if the value does not fit.
//! cites: 5.2
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := uint(-1);
      Console.WriteLine("no exception");
    catch
      OverflowException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
