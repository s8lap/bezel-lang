//! case: l9_1_4612
//! doc: stdlib
//! section: 9.1
//! title: int, long and ulong Parse raise OverflowException one past the type's maximum
//! rule: a value outside the type's range raising `ParseRangeException`
//! cites: 9.1
//! expect: run
//! output: int overflow
//!   long overflow
//!   ulong overflow
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("2147483648");
      Console.WriteLine("int none");
    catch
      OverflowException => Console.WriteLine("int overflow"),
    end try;
    try
      _ := long.Parse("9223372036854775808");
      Console.WriteLine("long none");
    catch
      OverflowException => Console.WriteLine("long overflow"),
    end try;
    try
      _ := ulong.Parse("18446744073709551616");
      Console.WriteLine("ulong none");
    catch
      OverflowException => Console.WriteLine("ulong overflow"),
    end try;
  end Main;
end Program;
