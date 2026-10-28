//! case: l9_3_0020
//! doc: stdlib
//! section: 9.3
//! title: a value whose rounding does not fit at scale 0 raises OverflowException
//! rule: stdlib §9.3 (v0.345): "`"79228162514264337593543950335.5"` raises — raises
//!   `OverflowException`".
//! cites: 9.3
//! expect: run
//! stdout: OVERFLOW
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse("79228162514264337593543950335.5"));
    catch
      OverflowException => Console.WriteLine("OVERFLOW");
    end;
  end Main;
end Program;
