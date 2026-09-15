//! case: l2_12_0005
//! doc: stdlib
//! section: 2.12
//! title: a NaN bit pattern is preserved exactly through a round trip
//! rule: For NaN values, the specific bit pattern (signaling vs quiet, payload
//!   bits) is preserved -- not merely a canonical NaN value.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var bits := 2143289345;
    var f := BitConverter.Int32BitsToFloat(bits);
    var back := BitConverter.FloatToInt32Bits(f);
    Console.WriteLine(back = bits);
  end Main;
end Program;
