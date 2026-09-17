//! case: s5_2_0010
//! doc: spec
//! section: 5.2
//! title: converting an out-of-range binary float to decimal raises OverflowException
//! rule: decimal(x) for binary-float x yields the nearest representable
//!   decimal value; NaN, +/-Infinity, and magnitudes outside decimal's range
//!   raise OverflowException.
//! cites: 5.2, 6.4
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var d: double := double.MaxValue;
      var m := decimal(d);
      Console.WriteLine(m);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
