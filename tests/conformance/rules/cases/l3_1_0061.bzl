//! case: l3_1_0061
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Replace raises ArgumentException for an empty oldValue passed by name in swapped order
//! rule: "`Replace` raises `ArgumentException` when `oldValue` is empty." The
//!   parameters are `oldValue` and `newValue`, so the empty value is the
//!   `oldValue` whatever the order the named arguments are written in.
//! cites: 3.1, 22.5
//! expect: run
//! output: ArgumentException
//!   abc
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    try
      _ := sb.Replace(newValue := "x", oldValue := "");
      Console.WriteLine("no exception");
    catch ArgumentException =>
      Console.WriteLine("ArgumentException");
    end;
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
