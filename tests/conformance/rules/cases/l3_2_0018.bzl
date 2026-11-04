//! case: l3_2_0018
//! doc: stdlib
//! section: 3.2
//! title: GetBytes into a destination span through an Encoding-typed value returns the bytes written
//! rule: public abstract method GetBytes(s: string, destination: Span<byte>): int;  // returns bytes written;
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: 4
//!   4

module Conf;

public class Program
  public static method Main()
    var e: Encoding := Encoding.UTF16;
    using var buf := new OwningBuffer<byte>(16);
    Console.WriteLine(e.GetBytes("hé", buf.AsSpan()));
    var sp := "hé".AsSpan();
    Console.WriteLine(e.GetBytes(sp, buf.AsSpan()));
  end Main;
end Program;
