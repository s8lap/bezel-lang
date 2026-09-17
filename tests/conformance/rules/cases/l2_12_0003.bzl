//! case: l2_12_0003
//! doc: stdlib
//! section: 2.12
//! title: Reinterpret views the same bits under a different type, not a value
//! rule: Reinterpret copies the value's bits unchanged -- it never raises and
//!   never alters the bit pattern, unlike a value-converting cast.
//! expect: run
//! stdout: 4294967295
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(BitConverter.Reinterpret<uint>(-1));
  end Main;
end Program;
