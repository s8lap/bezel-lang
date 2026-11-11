//! case: l9_1_0007
//! doc: stdlib
//! section: 9.1
//! title: int.Parse raises an OverflowException on an out-of-range value
//! rule: any other input raises `FormatException`, a value outside the type's
//!   range raising `OverflowException`. … The span overloads (#1810) accept
//!   exactly the inputs the string forms accept — same
//!   `FormatException`/`OverflowException` contract, no whitespace tolerance
//! cites: 9.1, 1
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("99999999999");
    catch
      OverflowException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
