//! case: l2_12_0001
//! doc: stdlib
//! section: 2.12
//! title: FloatToInt32Bits/Int32BitsToFloat round-trip a float exactly
//! rule: Int32BitsToFloat interprets the integer's bit pattern as a float;
//!   the bit patterns are preserved exactly, so FloatToInt32Bits followed by
//!   Int32BitsToFloat reproduces the original value.
//! expect: run
//! stdout: 1.5
module Conf;

public class Program
  public static method Main()
    var bits := BitConverter.FloatToInt32Bits(1.5f);
    Console.WriteLine(BitConverter.Int32BitsToFloat(bits));
  end Main;
end Program;
