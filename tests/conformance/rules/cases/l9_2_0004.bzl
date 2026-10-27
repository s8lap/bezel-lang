//! case: l9_2_0004
//! doc: stdlib
//! section: 9.2
//! title: double.Parse raises FormatException for the spellings the input grammar forbids
//! rule: There is no surrounding whitespace, no digit separator, no hexadecimal
//!   form and no bare point, so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`, `".5"`
//!   and `"5."` raise `FormatException`, as does any other input apart from
//!   exactly `"NaN"`, `"Infinity"` and `"-Infinity"`.
//! cites: 9.2
//! expect: run
//! output: [ 5] -> FMT
//!   [5 ] -> FMT
//!   [1_000] -> FMT
//!   [0x1p3] -> FMT
//!   [.5] -> FMT
//!   [5.] -> FMT

module Conf;

public class Program
  public static method Main()
    Show(" 5");
    Show("5 ");
    Show("1_000");
    Show("0x1p3");
    Show(".5");
    Show("5.");
  end Main;

  private static method Show(s: string)
    try
      _ := double.Parse(s);
      Console.WriteLine("[" + s + "] -> OK");
    catch
      FormatException => Console.WriteLine("[" + s + "] -> FMT");
    end;
  end Show;
end Program;
