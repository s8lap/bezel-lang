//! case: l9_1_0007
//! doc: stdlib
//! section: 9.1
//! title: int.Parse raises an OverflowException (ParseRangeException) on an out-of-range value
//! rule: A `Parse` member — here, in §9.2 and §9.3, and through §2.3's
//!   `Convert.To*` — whose input is well-formed and spells a value the type
//!   cannot represent raises `ParseRangeException` … It derives from
//!   `OverflowException` so that an `OverflowException` arm around a `Parse`
//!   call matches as it always has
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
