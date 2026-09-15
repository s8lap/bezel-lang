//! case: s5_2_0008
//! doc: spec
//! section: 5.2
//! title: casting an integer in the surrogate range to char raises OverflowException
//! rule: Casting an integer to char requires the integer to fall in
//!   U+0000–U+10FFFF excluding the surrogate range; otherwise
//!   OverflowException.
//! cites: 5.2, 16.3
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var n: int := 0xD800;
      var c := char(n);
      Console.WriteLine(c);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
