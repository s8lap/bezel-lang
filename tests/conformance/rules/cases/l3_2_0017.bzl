//! case: l3_2_0017
//! doc: stdlib
//! section: 3.2
//! title: a string-span source encodes through an Encoding-typed value over the span's code-point range
//! rule: identical contract to the string forms over the span's code-point range (`GetByteCount` and `GetBytes` agree
//! cites: 3.2, 3.3
//! expect: run
//! issue: 7028
//! output: 4
//!   4
//!   3

module Conf;

public class Program
  public static method Main()
    var s := "héllo";
    var sp := s.AsSpan(1, 2);
    var e16: Encoding := Encoding.UTF16;
    var e8: Encoding := Encoding.UTF8;
    Console.WriteLine(e16.GetByteCount(sp));
    using var b := e16.GetBytes(sp);
    Console.WriteLine(b.Count);
    Console.WriteLine(e8.GetByteCount(sp));
  end Main;
end Program;
