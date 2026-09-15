//! case: s18_5_0006
//! doc: spec
//! section: 18.5
//! title: alignment and a format spec compose as {expression,alignment:format}
//! rule: "`{expression,alignment:format}`" with example "`$\"{amount,12:F2}\"`" —
//!   the F2-formatted "1234.50" (7 characters) right-aligned in a 12-character
//!   field pads with 5 leading spaces.
//! cites: 18.5
//! expect: run
//! stdout: [     1234.50]
module Conf;

public class Program
  public static method Main()
    var amount: double := 1234.5;
    Console.WriteLine($"[{amount,12:F2}]");
  end Main;
end Program;
