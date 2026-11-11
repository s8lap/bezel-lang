//! case: l9_1_4806
//! doc: stdlib
//! section: 9.1
//! title: Array<byte>.Sort() orders values above 127 after the smaller ones (unsigned order)
//! rule: `SByte`, `Byte`, `Int16`, `UInt16`, `Int32`, `UInt32`, `Int64`, `UInt64`. The example below shows `Int32`; the others have analogous shapes with their respective `MinValue`/`MaxValue`.
//! cites: 4.4, 14.2
//! expect: run
//! output: 0
//!   127
//!   128
//!   200
//!   255
module Conf;

public class Program
  public static method Main()
    var a: Array<byte> := [200, 255, 0, 128, 127];
    a.Sort();
    Console.WriteLine(a[0]);
    Console.WriteLine(a[1]);
    Console.WriteLine(a[2]);
    Console.WriteLine(a[3]);
    Console.WriteLine(a[4]);
  end Main;
end Program;
