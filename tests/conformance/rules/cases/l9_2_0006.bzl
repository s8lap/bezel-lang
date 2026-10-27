//! case: l9_2_0006
//! doc: stdlib
//! section: 9.2
//! title: double.Parse raises FormatException for every non-finite spelling but the three exact ones
//! rule: They are matched exactly, so `"nan"`, `"inf"`, `"infinity"`, `"INFINITY"`,
//!   `"+Infinity"` and `"-NaN"` raise `FormatException`.
//! cites: 9.2, 2.18
//! expect: run
//! output: [nan] -> FMT
//!   [inf] -> FMT
//!   [infinity] -> FMT
//!   [INFINITY] -> FMT
//!   [+Infinity] -> FMT
//!   [-NaN] -> FMT

module Conf;

public class Program
  public static method Main()
    Show("nan");
    Show("inf");
    Show("infinity");
    Show("INFINITY");
    Show("+Infinity");
    Show("-NaN");
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
