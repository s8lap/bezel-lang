//! case: l9_2_0017
//! doc: stdlib
//! section: 9.2
//! title: Float.IeeeEquals accepts a long and an int through implicit conversions to float
//! rule: Float.IeeeEquals takes two float parameters; long and int both convert
//!   implicitly to float (language spec 5.1 table), and 4L equals 4 as floats.
//! cites: 5.1
//! expect: run
//! output: True
module Conf;

public class Program
  public static method Main()
    var l: long := 4L;
    Console.WriteLine(Float.IeeeEquals(l, 4));
  end Main;
end Program;
