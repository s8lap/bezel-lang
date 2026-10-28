//! case: l9_1_0022
//! doc: stdlib
//! section: 9.1
//! title: the radix parse form reaches the narrow integer types too
//! rule: stdlib §9.1 (v0.345): "Each integer type is declared as a record … The example below
//!   shows `Int32`; **the others have analogous shapes** with their respective
//!   `MinValue`/`MaxValue`." — the radix form is one of the analogous shapes.
//! cites: 9.1
//! expect: run
//! stdout: 15
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(byte.Parse("1111", 2));
  end Main;
end Program;
