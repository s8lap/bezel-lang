//! case: l9_6_0036
//! doc: stdlib
//! section: 9.6
//! title: an Insert at Length appends to the receiver
//! rule: stdlib §9.6 (v0.345) "The window members: `Substring`, `Remove` and `Insert`":
//!   "`startIndex` may equal `Length` … `"abc".Insert(3, "d")` is `"abcd"`".
//! cites: 9.6
//! expect: run
//! stdout: abcd
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abc".Insert(3, "d"));
  end Main;
end Program;
