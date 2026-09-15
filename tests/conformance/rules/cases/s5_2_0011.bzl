//! case: s5_2_0011
//! doc: spec
//! section: 5.2
//! title: converting decimal to double never fails
//! rule: double(m) and float(m) for decimal m round to the nearest
//!   representable binary value and never fail (float's and double's finite
//!   ranges exceed decimal's).
//! cites: 5.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var m: decimal := decimal.MaxValue;
    var d := double(m);
    Console.WriteLine(double.IsFinite(d));
  end Main;
end Program;
