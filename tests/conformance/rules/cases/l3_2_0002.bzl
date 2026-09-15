//! case: l3_2_0002
//! doc: stdlib
//! section: 3.2
//! title: ASCII.GetBytes raises FormatException above U+007F
//! rule: ASCII is strict seven-bit; GetBytes raises FormatException on any
//!   code point above U+007F.
//! cites: 3.2
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := Encoding.ASCII.GetBytes("café");
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
