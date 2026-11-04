//! case: l3_2_0013
//! doc: stdlib
//! section: 3.2
//! title: Encoding-typed UTF16 and UTF32 values measure and produce their little-endian forms
//! rule: little-endian, no BOM emitted
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: 8
//!   12
//!   8
//!   12

module Conf;

public class Program
  public static method Count(e: Encoding, s: string): int
    return e.GetByteCount(s);
  end Count;

  public static method Size(e: Encoding, s: string): int
    using var b := e.GetBytes(s);
    return b.Count;
  end Size;

  public static method Main()
    var s := "h😀é";
    var e16: Encoding := Encoding.UTF16;
    var e32: Encoding := Encoding.UTF32;
    Console.WriteLine(Count(e16, s));
    Console.WriteLine(Count(e32, s));
    Console.WriteLine(Size(e16, s));
    Console.WriteLine(Size(e32, s));
  end Main;
end Program;
