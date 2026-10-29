//! case: l3_1_0060
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Replace with an empty oldValue raises ArgumentException, builder unchanged
//! rule: "`Replace` raises `ArgumentException` when `oldValue` is empty."
//! cites: 3.1
//! expect: run
//! output: ArgumentException
//!   abc
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    try
      _ := sb.Replace("", "x");
      Console.WriteLine("no exception");
    catch ArgumentException =>
      Console.WriteLine("ArgumentException");
    end;
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
