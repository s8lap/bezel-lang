//! case: l9_1_3601
//! doc: stdlib
//! section: 9.1
//! title: a negative spelling for an unsigned width raises OverflowException
//! rule: The sign is in the grammar at every width, the unsigned ones included, so a negative spelling
//!   for an unsigned type is a value outside its range: `uint.Parse("-1")` and `ulong.Parse("-1", 10)`
//!   raise `OverflowException`, not `FormatException`.
//! cites: 9.1
//! expect: run
//! output: Overflow
//!   Overflow
//!   Overflow
//! issue: 8577
module Conf;

public class Program
  public static method Main()
    try
      var a := ulong.Parse("-1");
      Console.WriteLine("none");
    catch
      OverflowException => Console.WriteLine("Overflow"),
      FormatException => Console.WriteLine("Format"),
    end try;
    try
      var b := ulong.Parse("-1", 10);
      Console.WriteLine("none");
    catch
      OverflowException => Console.WriteLine("Overflow"),
      FormatException => Console.WriteLine("Format"),
    end try;
    try
      var c := uint.Parse("-1");
      Console.WriteLine("none");
    catch
      OverflowException => Console.WriteLine("Overflow"),
      FormatException => Console.WriteLine("Format"),
    end try;
  end Main;
end Program;
