//! case: l9_6_0035
//! doc: stdlib
//! section: 9.6
//! title: a to-the-end Remove window at Length yields the receiver unchanged
//! rule: stdlib §9.6 (v0.345) "The window members: `Substring`, `Remove` and `Insert`":
//!   "`startIndex` may equal `Length` and `length` may be `0`: … `"abc".Remove(3)` is `"abc"`".
//! cites: 9.6
//! expect: run
//! stdout: abc
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abc".Remove(3));
  end Main;
end Program;
