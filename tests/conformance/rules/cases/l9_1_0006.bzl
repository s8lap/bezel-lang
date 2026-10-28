//! case: l9_1_0006
//! doc: stdlib
//! section: 9.1
//! title: int.Parse with an unsupported radix raises ArgumentException
//! rule: radix in {2, 8, 10, 16}; any other radix raises
//!   ArgumentException — a caller bug, not input failure.
//! cites: 9.1
//! expect: run
//! stdout: caught
//! issue: 7268
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("10", 7);
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
