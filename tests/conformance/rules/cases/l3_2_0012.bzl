//! case: l3_2_0012
//! doc: stdlib
//! section: 3.2
//! title: GetByteCount and GetBytes through an Encoding-typed UTF8 value measure the UTF-8 form
//! rule: bytes are the UTF-8 form of the string's code points
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: 7
//!   7
//!   utf-8

module Conf;

public class Program
  public static method Size(e: Encoding, s: string): int
    using var b := e.GetBytes(s);
    return b.Count;
  end Size;

  public static method Main()
    var e: Encoding := Encoding.UTF8;
    var s := "h😀é";
    Console.WriteLine(e.GetByteCount(s));
    Console.WriteLine(Size(e, s));
    Console.WriteLine(e.Name);
  end Main;
end Program;
