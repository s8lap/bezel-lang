//! case: l5_0012
//! doc: stdlib
//! section: 5
//! title: Sum() accumulates in the element's own type and is overflow-checked in it
//! rule: Sum now returns the element's own type, so a narrow integer
//!   element type accumulates in that type and is overflow-checked in it --
//!   bytes.Sum() over an Array<byte> raises OverflowException past 255.
//! cites: 5, 6.1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var xs: Array<byte> := [byte(200), byte(200)];
    try
      var total := xs.Sum();
    catch
      OverflowException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
