//! case: l2_12_0002
//! doc: stdlib
//! section: 2.12
//! title: DoubleToInt64Bits/Int64BitsToDouble round-trip a double exactly
//! rule: These are pure bit-reinterpretation operations: the bit patterns are
//!   preserved exactly, so DoubleToInt64Bits followed by Int64BitsToDouble
//!   reproduces the original value.
//! expect: run
//! stdout: 3.14
module Conf;

public class Program
  public static method Main()
    var bits := BitConverter.DoubleToInt64Bits(3.14);
    Console.WriteLine(BitConverter.Int64BitsToDouble(bits));
  end Main;
end Program;
