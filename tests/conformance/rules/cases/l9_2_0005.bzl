//! case: l9_2_0005
//! doc: stdlib
//! section: 9.2
//! title: a signed bare point is not in the input grammar and raises FormatException
//! rule: **Input grammar.** `Parse` accepts an optional sign (`+` or `-`), then one
//!   or more of the digits `0`–`9`, then optionally a `.` followed by one or
//!   more digits, then optionally an exponent — `e` or `E`, an optional sign,
//!   and one or more digits — and nothing else: `"5"`, `"+5"`, `"-0.25"`,
//!   `"007.5"` and `"6.02E+23"` are admitted.
//! cites: 9.2
//! expect: run
//! output: [+.5] -> FMT
//!   [-.5] -> FMT

module Conf;

public class Program
  public static method Main()
    Show("+.5");
    Show("-.5");
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
